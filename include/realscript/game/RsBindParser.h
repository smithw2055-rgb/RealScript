#pragma once

#include "realscript/game/GameBindings.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realscript::game::rsbind_detail {

inline std::string trimView(std::string_view sv) {
    std::size_t b = 0;
    while (b < sv.size() &&
           (sv[b] == ' ' || sv[b] == '\t' || sv[b] == '\r' || sv[b] == '\n')) {
        ++b;
    }
    std::size_t e = sv.size();
    while (e > b &&
           (sv[e - 1] == ' ' || sv[e - 1] == '\t' ||
            sv[e - 1] == '\r' || sv[e - 1] == '\n')) {
        --e;
    }
    return std::string(sv.substr(b, e - b));
}

inline bool isIdentStart(char ch) noexcept {
    const auto u = static_cast<unsigned char>(ch);
    return std::isalpha(u) != 0 || ch == '_';
}

inline bool isIdentCont(char ch) noexcept {
    const auto u = static_cast<unsigned char>(ch);
    return std::isalnum(u) != 0 || ch == '_';
}

inline bool isReservedExportMacro(std::string_view name) noexcept {
    return name == "RS_EXPORT_CLASS" ||
        name == "RS_EXPORT_STRUCT" ||
        name == "RS_EXPORT_ECS_COMPONENT" ||
        name == "RS_EXPORT_ENUM" ||
        name == "RS_EXPORT_FIELD" ||
        name == "RS_EXPORT_PROPERTY" ||
        name == "RS_EXPORT_CONSTRUCTOR" ||
        name == "RS_EXPORT_METHOD" ||
        name == "RS_EXPORT_METHOD_EX" ||
        name == "RS_EXPORT_FUNCTION" ||
        name == "RS_EXPORT_FUNCTION_EX";
}

// ============================================================================
// 1. Lightweight Self-Contained C++ Preprocessor (#define / #ifdef / #if expr)
// ============================================================================
struct PpMacro {
    bool isFunctionLike = false;
    bool variadic = false;
    std::vector<std::string> params;
    std::string body;
};

inline std::size_t tryScanRawStringEnd(std::string_view src, std::size_t rPos) noexcept {
    if (rPos + 2 >= src.size() || src[rPos] != 'R' || src[rPos + 1] != '"') {
        return std::string_view::npos;
    }
    const std::size_t dStart = rPos + 2;
    std::size_t dEnd = dStart;
    while (dEnd < src.size() && src[dEnd] != '(' &&
           src[dEnd] != ' ' && src[dEnd] != '\t' &&
           src[dEnd] != '\r' && src[dEnd] != '\n' &&
           src[dEnd] != '\\' && src[dEnd] != ')' &&
           (dEnd - dStart) <= 16) {
        ++dEnd;
    }
    if (dEnd >= src.size() || src[dEnd] != '(') {
        return std::string_view::npos;
    }
    const std::string_view delim = src.substr(dStart, dEnd - dStart);
    std::size_t i = dEnd + 1;
    while (i < src.size()) {
        if (src[i] == ')' &&
            i + 1 + delim.size() < src.size() &&
            src.substr(i + 1, delim.size()) == delim &&
            src[i + 1 + delim.size()] == '"') {
            return i + 2 + delim.size();
        }
        ++i;
    }
    return src.size();
}

inline std::size_t tryScanAnyRawStringEnd(std::string_view src, std::size_t pos) noexcept {
    if (pos >= src.size()) return std::string_view::npos;
    if (src[pos] == 'R' && pos + 1 < src.size() && src[pos + 1] == '"') {
        return tryScanRawStringEnd(src, pos);
    }
    if ((src[pos] == 'L' || src[pos] == 'u' || src[pos] == 'U') &&
        pos + 2 < src.size() && src[pos + 1] == 'R' && src[pos + 2] == '"') {
        return tryScanRawStringEnd(src, pos + 1);
    }
    if (src[pos] == 'u' && pos + 3 < src.size() &&
        src[pos + 1] == '8' && src[pos + 2] == 'R' && src[pos + 3] == '"') {
        return tryScanRawStringEnd(src, pos + 2);
    }
    return std::string_view::npos;
}

inline std::string stringizeMacroArg(std::string_view raw) {
    const std::string trimmed = trimView(raw);
    std::string res;
    res.reserve(trimmed.size() + 4);
    res.push_back('"');
    bool inWs = false;
    for (std::size_t i = 0; i < trimmed.size();) {
        if (i == 0 || !isIdentCont(trimmed[i - 1])) {
            const std::size_t rawEnd = tryScanAnyRawStringEnd(trimmed, i);
            if (rawEnd != std::string_view::npos) {
                if (inWs) {
                    res.push_back(' ');
                    inWs = false;
                }
                for (std::size_t k = i; k < rawEnd; ++k) {
                    if (trimmed[k] == '"') res += "\\\"";
                    else if (trimmed[k] == '\\') res += "\\\\";
                    else res.push_back(trimmed[k]);
                }
                i = rawEnd;
                continue;
            }
        }
        const char c = trimmed[i];
        if (c == '"' || c == '\'') {
            if (inWs) {
                res.push_back(' ');
                inWs = false;
            }
            const char q = c;
            if (q == '"') res += "\\\"";
            else res.push_back('\'');
            ++i;
            while (i < trimmed.size()) {
                if (trimmed[i] == '\\' && i + 1 < trimmed.size()) {
                    res += "\\\\";
                    if (trimmed[i + 1] == '"') res += "\\\"";
                    else if (trimmed[i + 1] == '\\') res += "\\\\";
                    else res.push_back(trimmed[i + 1]);
                    i += 2;
                    continue;
                }
                const char qc = trimmed[i++];
                if (qc == '"') res += "\\\"";
                else if (qc == '\\') res += "\\\\";
                else res.push_back(qc);
                if (qc == q) break;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            inWs = true;
            ++i;
            continue;
        }
        if (inWs) {
            res.push_back(' ');
            inWs = false;
        }
        res.push_back(c);
        ++i;
    }
    res.push_back('"');
    return res;
}

inline std::string substituteMacroBody(
    std::string_view body,
    bool isFunctionLike,
    const std::unordered_map<std::string, std::string>& rawParamMap,
    const std::unordered_map<std::string, std::string>& expandedParamMap) {
    enum class BodyItemKind : std::uint8_t { Ws, Lit, Hash, HashHash, Ident, Other };
    struct BodyItem {
        BodyItemKind kind = BodyItemKind::Other;
        std::string text;
        bool wasEmptyVaArgs = false;
    };

    std::vector<BodyItem> items;
    items.reserve(body.size() / 2 + 4);
    for (std::size_t bi = 0; bi < body.size();) {
        if (bi == 0 || !isIdentCont(body[bi - 1])) {
            const std::size_t rawEnd = tryScanAnyRawStringEnd(body, bi);
            if (rawEnd != std::string_view::npos) {
                items.push_back(BodyItem{BodyItemKind::Lit, std::string(body.substr(bi, rawEnd - bi))});
                bi = rawEnd;
                continue;
            }
        }
        if (body[bi] == '"' || body[bi] == '\'') {
            const char q = body[bi];
            const std::size_t sStart = bi++;
            while (bi < body.size()) {
                if (body[bi] == '\\' && bi + 1 < body.size()) {
                    bi += 2;
                    continue;
                }
                if (body[bi++] == q) break;
            }
            items.push_back(BodyItem{BodyItemKind::Lit, std::string(body.substr(sStart, bi - sStart))});
            continue;
        }
        if (body[bi] == ' ' || body[bi] == '\t' || body[bi] == '\r' || body[bi] == '\n') {
            const std::size_t wStart = bi++;
            while (bi < body.size() &&
                   (body[bi] == ' ' || body[bi] == '\t' || body[bi] == '\r' || body[bi] == '\n')) {
                ++bi;
            }
            items.push_back(BodyItem{BodyItemKind::Ws, std::string(body.substr(wStart, bi - wStart))});
            continue;
        }
        if (body[bi] == '#') {
            if (bi + 1 < body.size() && body[bi + 1] == '#') {
                items.push_back(BodyItem{BodyItemKind::HashHash, "##"});
                bi += 2;
            } else {
                items.push_back(BodyItem{BodyItemKind::Hash, "#"});
                ++bi;
            }
            continue;
        }
        if (isIdentStart(body[bi])) {
            const std::size_t iStart = bi++;
            while (bi < body.size() && isIdentCont(body[bi])) ++bi;
            items.push_back(BodyItem{BodyItemKind::Ident, std::string(body.substr(iStart, bi - iStart))});
            continue;
        }
        items.push_back(BodyItem{BodyItemKind::Other, std::string(1, body[bi++])});
    }

    if (isFunctionLike) {
        // Pass 1: #param stringizing
        for (std::size_t k = 0; k < items.size(); ++k) {
            if (items[k].kind != BodyItemKind::Hash) continue;
            std::size_t m = k + 1;
            while (m < items.size() && items[m].kind == BodyItemKind::Ws) ++m;
            if (m < items.size() && items[m].kind == BodyItemKind::Ident) {
                const auto rIt = rawParamMap.find(items[m].text);
                if (rIt != rawParamMap.end()) {
                    items[k] = BodyItem{BodyItemKind::Lit, stringizeMacroArg(rIt->second)};
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(k + 1),
                                items.begin() + static_cast<std::ptrdiff_t>(m + 1));
                }
            }
        }

        // Pass 2: parameter substitution (raw when adjacent to ##, macro-expanded otherwise)
        for (std::size_t k = 0; k < items.size(); ++k) {
            if (items[k].kind != BodyItemKind::Ident) continue;
            bool leftPaste = false;
            for (std::size_t l = k; l > 0;) {
                --l;
                if (items[l].kind == BodyItemKind::Ws) continue;
                leftPaste = (items[l].kind == BodyItemKind::HashHash);
                break;
            }
            bool rightPaste = false;
            for (std::size_t r = k + 1; r < items.size(); ++r) {
                if (items[r].kind == BodyItemKind::Ws) continue;
                rightPaste = (items[r].kind == BodyItemKind::HashHash);
                break;
            }
            if (leftPaste || rightPaste) {
                const auto rIt = rawParamMap.find(items[k].text);
                if (rIt != rawParamMap.end()) {
                    const bool emptyVa = (items[k].text == "__VA_ARGS__" && rIt->second.empty());
                    items[k] = BodyItem{BodyItemKind::Lit, rIt->second, emptyVa};
                }
            } else {
                const auto eIt = expandedParamMap.find(items[k].text);
                if (eIt != expandedParamMap.end()) {
                    items[k] = BodyItem{BodyItemKind::Lit, " " + eIt->second + " "};
                }
            }
        }
    }

    // Pass 3: ## token pasting (also supports `, ## __VA_ARGS__` empty variadic comma elision)
    std::string out;
    out.reserve(body.size() + 16);
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items[k].kind == BodyItemKind::Ws) {
            bool beforePaste = false;
            for (std::size_t r = k + 1; r < items.size(); ++r) {
                if (items[r].kind == BodyItemKind::Ws) continue;
                beforePaste = (items[r].kind == BodyItemKind::HashHash);
                break;
            }
            if (beforePaste) continue;
            out += items[k].text;
            continue;
        }
        if (items[k].kind == BodyItemKind::HashHash) {
            std::size_t r = k + 1;
            while (r < items.size() && items[r].kind == BodyItemKind::Ws) ++r;
            if (r < items.size() && items[r].wasEmptyVaArgs) {
                // Trim trailing comma for `, ## __VA_ARGS__`
                if (!out.empty() && out.back() == ',') {
                    out.pop_back();
                }
                k = r;
                continue;
            }
            if (r > k + 1) {
                k = r - 1; // skip whitespace immediately following ##
            }
            continue;
        }
        out += items[k].text;
    }
    return out;
}

inline std::string expandMacrosBounded(
    std::string_view text,
    const std::unordered_map<std::string, PpMacro>& macros,
    bool preserveDefinedOp,
    std::size_t& remainingSteps,
    const std::unordered_set<std::string>& activeSet = {}) {
    if (macros.empty() || remainingSteps == 0) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        if (remainingSteps == 0) {
            out.append(text.substr(i));
            break;
        }
        if (i == 0 || !isIdentCont(text[i - 1])) {
            const std::size_t rawEnd = tryScanAnyRawStringEnd(text, i);
            if (rawEnd != std::string_view::npos) {
                out.append(text.substr(i, rawEnd - i));
                i = rawEnd;
                continue;
            }
        }
        if (text[i] == '"' || text[i] == '\'') {
            const char q = text[i];
            out.push_back(text[i++]);
            while (i < text.size()) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    out.push_back(text[i++]);
                    out.push_back(text[i++]);
                    continue;
                }
                const char c = text[i++];
                out.push_back(c);
                if (c == q) break;
            }
            continue;
        }
        if (isIdentStart(text[i])) {
            const std::size_t start = i++;
            while (i < text.size() && isIdentCont(text[i])) ++i;
            const std::string_view identView = text.substr(start, i - start);

            if (preserveDefinedOp && identView == "defined") {
                out.append(identView);
                while (i < text.size() &&
                       (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
                    out.push_back(text[i++]);
                }
                if (i < text.size() && text[i] == '(') {
                    out.push_back(text[i++]);
                    while (i < text.size() && text[i] != ')') {
                        out.push_back(text[i++]);
                    }
                    if (i < text.size() && text[i] == ')') {
                        out.push_back(text[i++]);
                    }
                } else if (i < text.size() && isIdentStart(text[i])) {
                    while (i < text.size() && isIdentCont(text[i])) {
                        out.push_back(text[i++]);
                    }
                }
                continue;
            }

            const std::string ident(identView);
            if (!isReservedExportMacro(identView) && activeSet.find(ident) == activeSet.end()) {
                const auto it = macros.find(ident);
                if (it != macros.end()) {
                    const PpMacro& mac = it->second;
                    if (!mac.isFunctionLike) {
                        --remainingSteps;
                        auto nextActive = activeSet;
                        nextActive.insert(ident);
                        out.push_back(' ');
                        if (!mac.body.empty()) {
                            const std::string pasted =
                                mac.body.find("##") != std::string::npos
                                    ? substituteMacroBody(mac.body, false, {}, {})
                                    : mac.body;
                            out += expandMacrosBounded(
                                pasted, macros, preserveDefinedOp, remainingSteps, nextActive);
                        }
                        out.push_back(' ');
                        continue;
                    }

                    // Function-like macro: expand ONLY when followed by '('
                    std::size_t scan = i;
                    while (scan < text.size() &&
                           (text[scan] == ' ' || text[scan] == '\t' ||
                            text[scan] == '\r' || text[scan] == '\n')) {
                        ++scan;
                    }
                    if (scan < text.size() && text[scan] == '(') {
                        std::size_t argCursor = scan + 1;
                        std::vector<std::string> callArgs;
                        std::string currentArg;
                        int pDepth = 1;
                        while (argCursor < text.size() && pDepth > 0) {
                            if (argCursor == scan + 1 || !isIdentCont(text[argCursor - 1])) {
                                const std::size_t rawEnd = tryScanAnyRawStringEnd(text, argCursor);
                                if (rawEnd != std::string_view::npos) {
                                    currentArg.append(text.substr(argCursor, rawEnd - argCursor));
                                    argCursor = rawEnd;
                                    continue;
                                }
                            }
                            const char ch = text[argCursor];
                            if (ch == '"' || ch == '\'') {
                                const char q = ch;
                                currentArg.push_back(text[argCursor++]);
                                while (argCursor < text.size()) {
                                    if (text[argCursor] == '\\' && argCursor + 1 < text.size()) {
                                        currentArg.push_back(text[argCursor++]);
                                        currentArg.push_back(text[argCursor++]);
                                        continue;
                                    }
                                    const char qc = text[argCursor++];
                                    currentArg.push_back(qc);
                                    if (qc == q) break;
                                }
                                continue;
                            }
                            if (ch == '(') {
                                ++pDepth;
                                currentArg.push_back(ch);
                                ++argCursor;
                            } else if (ch == ')') {
                                --pDepth;
                                if (pDepth == 0) {
                                    ++argCursor;
                                    break;
                                }
                                currentArg.push_back(ch);
                                ++argCursor;
                            } else if (ch == ',' && pDepth == 1) {
                                callArgs.push_back(trimView(currentArg));
                                currentArg.clear();
                                ++argCursor;
                            } else {
                                currentArg.push_back(ch);
                                ++argCursor;
                            }
                        }
                        if (pDepth == 0) {
                            const std::string lastTrimmed = trimView(currentArg);
                            if (!lastTrimmed.empty() || !callArgs.empty()) {
                                callArgs.push_back(lastTrimmed);
                            }
                            i = argCursor;
                            --remainingSteps;
                            if (mac.body.empty()) {
                                out.push_back(' ');
                                continue;
                            }
                            std::unordered_map<std::string, std::string> rawParamMap;
                            std::unordered_map<std::string, std::string> expandedParamMap;
                            for (std::size_t pi = 0; pi < mac.params.size(); ++pi) {
                                const std::string rawVal = pi < callArgs.size() ? callArgs[pi] : std::string{};
                                rawParamMap[mac.params[pi]] = rawVal;
                                expandedParamMap[mac.params[pi]] = expandMacrosBounded(
                                    rawVal, macros, preserveDefinedOp, remainingSteps, activeSet);
                            }
                            if (mac.variadic) {
                                std::string rawVa;
                                std::string expVa;
                                for (std::size_t vi = mac.params.size(); vi < callArgs.size(); ++vi) {
                                    if (!rawVa.empty()) rawVa += ", ";
                                    rawVa += callArgs[vi];
                                    if (!expVa.empty()) expVa += ", ";
                                    expVa += expandMacrosBounded(
                                        callArgs[vi], macros, preserveDefinedOp, remainingSteps, activeSet);
                                }
                                rawParamMap["__VA_ARGS__"] = std::move(rawVa);
                                expandedParamMap["__VA_ARGS__"] = std::move(expVa);
                            }

                            const std::string substituted = substituteMacroBody(
                                mac.body, true, rawParamMap, expandedParamMap);

                            auto nextActive = activeSet;
                            nextActive.insert(ident);
                            out.push_back(' ');
                            out += expandMacrosBounded(
                                substituted, macros, preserveDefinedOp, remainingSteps, nextActive);
                            out.push_back(' ');
                            continue;
                        }
                    }
                }
            }
            out.append(identView);
            continue;
        }
        out.push_back(text[i++]);
    }
    return out;
}

class CppConstExprEvaluator {
public:
    static constexpr std::size_t kMaxEvalSteps = 65536;

    CppConstExprEvaluator(
        std::string_view expr,
        const std::unordered_map<std::string, PpMacro>* macros,
        const std::unordered_map<std::string, std::int64_t>* enumValues = nullptr)
        : src_(expr), macros_(macros), enumValues_(enumValues) {}

    [[nodiscard]] std::int64_t evaluate(bool* ok = nullptr) {
        pos_ = 0;
        valid_ = true;
        suppressDepth_ = 0;
        steps_ = 0;
        const EvalVal val = parseTernary();
        skipWs();
        if (pos_ < src_.size()) {
            valid_ = false;
        }
        if (ok) *ok = valid_;
        return val.s();
    }

private:
    struct EvalVal {
        std::uint64_t bits = 0;
        bool isUnsigned = false;

        [[nodiscard]] bool truthy() const noexcept { return bits != 0; }
        [[nodiscard]] std::int64_t s() const noexcept {
            return static_cast<std::int64_t>(bits);
        }
    };

    std::string_view src_;
    const std::unordered_map<std::string, PpMacro>* macros_ = nullptr;
    const std::unordered_map<std::string, std::int64_t>* enumValues_ = nullptr;
    std::size_t pos_ = 0;
    bool valid_ = true;
    int depth_ = 0;
    int suppressDepth_ = 0;
    std::size_t steps_ = 0;

    bool stepGuard() noexcept {
        if (++steps_ > kMaxEvalSteps) {
            valid_ = false;
            pos_ = src_.size();
            return false;
        }
        return true;
    }

    void markInvalid() noexcept {
        if (suppressDepth_ == 0) {
            valid_ = false;
        }
    }

    void skipWs() noexcept {
        while (pos_ < src_.size() &&
               (src_[pos_] == ' ' || src_[pos_] == '\t' ||
                src_[pos_] == '\r' || src_[pos_] == '\n')) {
            ++pos_;
        }
    }

    bool match(std::string_view tok) noexcept {
        skipWs();
        if (src_.substr(pos_).rfind(tok, 0) == 0) {
            pos_ += tok.size();
            return true;
        }
        return false;
    }

    void skipBalancedParenInSrc() noexcept {
        skipWs();
        if (pos_ >= src_.size() || src_[pos_] != '(') return;
        int pDepth = 0;
        while (pos_ < src_.size()) {
            const char ch = src_[pos_++];
            if (ch == '"' || ch == '\'') {
                const char q = ch;
                while (pos_ < src_.size()) {
                    if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                        pos_ += 2;
                        continue;
                    }
                    if (src_[pos_++] == q) break;
                }
                continue;
            }
            if (ch == '(') ++pDepth;
            else if (ch == ')') {
                --pDepth;
                if (pDepth <= 0) return;
            }
        }
    }

    EvalVal parseTernary() {
        if (!stepGuard()) return {};
        auto cond = parseLogicalOr();
        skipWs();
        if (pos_ < src_.size() && src_[pos_] == '?') {
            ++pos_;
            if (suppressDepth_ > 0) {
                (void)parseTernary();
                if (!match(":")) markInvalid();
                (void)parseTernary();
                return {};
            }
            if (cond.truthy()) {
                const auto tVal = parseTernary();
                if (!match(":")) {
                    markInvalid();
                    return tVal;
                }
                ++suppressDepth_;
                (void)parseTernary();
                --suppressDepth_;
                return tVal;
            }
            ++suppressDepth_;
            (void)parseTernary();
            --suppressDepth_;
            if (!match(":")) {
                markInvalid();
                return cond;
            }
            return parseTernary();
        }
        return cond;
    }

    EvalVal parseLogicalOr() {
        if (!stepGuard()) return {};
        auto lhs = parseLogicalAnd();
        for (;;) {
            skipWs();
            if (pos_ + 1 < src_.size() && src_[pos_] == '|' && src_[pos_ + 1] == '|') {
                pos_ += 2;
                if (suppressDepth_ > 0 || lhs.truthy()) {
                    ++suppressDepth_;
                    (void)parseLogicalAnd();
                    --suppressDepth_;
                    if (suppressDepth_ == 0) {
                        lhs = EvalVal{1ull, false};
                    }
                } else {
                    const auto rhs = parseLogicalAnd();
                    lhs = EvalVal{rhs.truthy() ? 1ull : 0ull, false};
                }
            } else {
                break;
            }
        }
        return lhs;
    }

    EvalVal parseLogicalAnd() {
        if (!stepGuard()) return {};
        auto lhs = parseBitOr();
        for (;;) {
            skipWs();
            if (pos_ + 1 < src_.size() && src_[pos_] == '&' && src_[pos_ + 1] == '&') {
                pos_ += 2;
                if (suppressDepth_ > 0 || !lhs.truthy()) {
                    ++suppressDepth_;
                    (void)parseBitOr();
                    --suppressDepth_;
                    if (suppressDepth_ == 0) {
                        lhs = EvalVal{0ull, false};
                    }
                } else {
                    const auto rhs = parseBitOr();
                    lhs = EvalVal{rhs.truthy() ? 1ull : 0ull, false};
                }
            } else {
                break;
            }
        }
        return lhs;
    }

    EvalVal parseBitOr() {
        auto lhs = parseBitXor();
        for (;;) {
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == '|' &&
                (pos_ + 1 >= src_.size() || src_[pos_ + 1] != '|')) {
                ++pos_;
                const auto rhs = parseBitXor();
                lhs = EvalVal{lhs.bits | rhs.bits, lhs.isUnsigned || rhs.isUnsigned};
            } else {
                break;
            }
        }
        return lhs;
    }

    EvalVal parseBitXor() {
        auto lhs = parseBitAnd();
        while (match("^")) {
            const auto rhs = parseBitAnd();
            lhs = EvalVal{lhs.bits ^ rhs.bits, lhs.isUnsigned || rhs.isUnsigned};
        }
        return lhs;
    }

    EvalVal parseBitAnd() {
        auto lhs = parseEquality();
        for (;;) {
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == '&' &&
                (pos_ + 1 >= src_.size() || src_[pos_ + 1] != '&')) {
                ++pos_;
                const auto rhs = parseEquality();
                lhs = EvalVal{lhs.bits & rhs.bits, lhs.isUnsigned || rhs.isUnsigned};
            } else {
                break;
            }
        }
        return lhs;
    }

    EvalVal parseEquality() {
        auto lhs = parseRelational();
        for (;;) {
            if (match("==")) {
                const auto rhs = parseRelational();
                lhs = EvalVal{(lhs.bits == rhs.bits) ? 1ull : 0ull, false};
            } else if (match("!=")) {
                const auto rhs = parseRelational();
                lhs = EvalVal{(lhs.bits != rhs.bits) ? 1ull : 0ull, false};
            } else {
                break;
            }
        }
        return lhs;
    }

    EvalVal parseRelational() {
        auto lhs = parseShift();
        for (;;) {
            const auto cmp = [](EvalVal a, EvalVal b, int op) noexcept -> EvalVal {
                const bool u = a.isUnsigned || b.isUnsigned;
                bool r = false;
                switch (op) {
                case 0: r = u ? (a.bits <= b.bits) : (a.s() <= b.s()); break;
                case 1: r = u ? (a.bits >= b.bits) : (a.s() >= b.s()); break;
                case 2: r = u ? (a.bits < b.bits) : (a.s() < b.s()); break;
                case 3: r = u ? (a.bits > b.bits) : (a.s() > b.s()); break;
                default: break;
                }
                return EvalVal{r ? 1ull : 0ull, false};
            };
            skipWs();
            if (match("<=")) lhs = cmp(lhs, parseShift(), 0);
            else if (match(">=")) lhs = cmp(lhs, parseShift(), 1);
            else if (pos_ + 1 < src_.size() && src_[pos_] == '<' && src_[pos_ + 1] == '<') break;
            else if (pos_ + 1 < src_.size() && src_[pos_] == '>' && src_[pos_ + 1] == '>') break;
            else if (match("<")) lhs = cmp(lhs, parseShift(), 2);
            else if (match(">")) lhs = cmp(lhs, parseShift(), 3);
            else break;
        }
        return lhs;
    }

    EvalVal parseShift() {
        auto lhs = parseAdditive();
        for (;;) {
            if (match("<<")) {
                const auto rhs = parseAdditive();
                const bool okShift = rhs.isUnsigned
                    ? (rhs.bits < 64u)
                    : (rhs.s() >= 0 && rhs.s() < 64);
                lhs = EvalVal{okShift ? (lhs.bits << (rhs.bits & 63u)) : 0ull, lhs.isUnsigned};
            } else if (match(">>")) {
                const auto rhs = parseAdditive();
                const bool okShift = rhs.isUnsigned
                    ? (rhs.bits < 64u)
                    : (rhs.s() >= 0 && rhs.s() < 64);
                if (!okShift) {
                    lhs = EvalVal{lhs.isUnsigned ? 0ull : (lhs.s() < 0 ? ~0ull : 0ull), lhs.isUnsigned};
                } else if (lhs.isUnsigned) {
                    lhs = EvalVal{lhs.bits >> (rhs.bits & 63u), true};
                } else {
                    lhs = EvalVal{static_cast<std::uint64_t>(lhs.s() >> (rhs.bits & 63u)), false};
                }
            } else {
                break;
            }
        }
        return lhs;
    }

    EvalVal parseAdditive() {
        auto lhs = parseMultiplicative();
        for (;;) {
            if (match("+")) {
                const auto rhs = parseMultiplicative();
                lhs = EvalVal{lhs.bits + rhs.bits, lhs.isUnsigned || rhs.isUnsigned};
            } else if (match("-")) {
                const auto rhs = parseMultiplicative();
                lhs = EvalVal{lhs.bits - rhs.bits, lhs.isUnsigned || rhs.isUnsigned};
            } else {
                break;
            }
        }
        return lhs;
    }

    EvalVal parseMultiplicative() {
        auto lhs = parseUnary();
        for (;;) {
            if (match("*")) {
                const auto rhs = parseUnary();
                lhs = EvalVal{lhs.bits * rhs.bits, lhs.isUnsigned || rhs.isUnsigned};
            } else if (match("/")) {
                const auto rhs = parseUnary();
                const bool u = lhs.isUnsigned || rhs.isUnsigned;
                if (suppressDepth_ > 0) {
                    lhs = EvalVal{0ull, u};
                } else if (rhs.bits == 0) {
                    if (enumValues_ && !macros_) markInvalid();
                    lhs = EvalVal{0ull, u};
                } else if (u) {
                    lhs = EvalVal{lhs.bits / rhs.bits, true};
                } else if (lhs.s() == std::numeric_limits<std::int64_t>::min() && rhs.s() == -1) {
                    lhs = EvalVal{static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::min()), false};
                } else {
                    lhs = EvalVal{static_cast<std::uint64_t>(lhs.s() / rhs.s()), false};
                }
            } else if (match("%")) {
                const auto rhs = parseUnary();
                const bool u = lhs.isUnsigned || rhs.isUnsigned;
                if (suppressDepth_ > 0) {
                    lhs = EvalVal{0ull, u};
                } else if (rhs.bits == 0) {
                    if (enumValues_ && !macros_) markInvalid();
                    lhs = EvalVal{0ull, u};
                } else if (u) {
                    lhs = EvalVal{lhs.bits % rhs.bits, true};
                } else if (lhs.s() == std::numeric_limits<std::int64_t>::min() && rhs.s() == -1) {
                    lhs = EvalVal{0ull, false};
                } else {
                    lhs = EvalVal{static_cast<std::uint64_t>(lhs.s() % rhs.s()), false};
                }
            } else {
                break;
            }
        }
        return lhs;
    }

    EvalVal parseUnary() {
        if (!stepGuard()) return {};
        skipWs();
        if (pos_ >= src_.size()) {
            markInvalid();
            return {};
        }
        if (src_[pos_] == '+') { ++pos_; return parseUnary(); }
        if (src_[pos_] == '-') {
            ++pos_;
            const auto v = parseUnary();
            return EvalVal{0ull - v.bits, v.isUnsigned};
        }
        if (src_[pos_] == '!') {
            if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '=') return parsePrimary();
            ++pos_;
            return EvalVal{parseUnary().truthy() ? 0ull : 1ull, false};
        }
        if (src_[pos_] == '~') {
            ++pos_;
            const auto v = parseUnary();
            return EvalVal{~v.bits, v.isUnsigned};
        }
        return parsePrimary();
    }

    std::string readIdentifier() {
        skipWs();
        if (pos_ >= src_.size() || !isIdentStart(src_[pos_])) return {};
        const std::size_t start = pos_++;
        while (pos_ < src_.size() && isIdentCont(src_[pos_])) ++pos_;
        return std::string(src_.substr(start, pos_ - start));
    }

    EvalVal parseCharLiteralAfterQuote() {
        std::uint64_t chVal = 0;
        if (pos_ < src_.size() && src_[pos_] != '\'') {
            if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                ++pos_;
                const char esc = src_[pos_++];
                switch (esc) {
                case 'a': chVal = '\a'; break;
                case 'b': chVal = '\b'; break;
                case 'f': chVal = '\f'; break;
                case 'n': chVal = '\n'; break;
                case 'r': chVal = '\r'; break;
                case 't': chVal = '\t'; break;
                case 'v': chVal = '\v'; break;
                case '\\': chVal = '\\'; break;
                case '\'': chVal = '\''; break;
                case '"': chVal = '"'; break;
                case '?': chVal = '?'; break;
                case 'x': {
                    if (pos_ >= src_.size() ||
                        std::isxdigit(static_cast<unsigned char>(src_[pos_])) == 0) {
                        markInvalid();
                        break;
                    }
                    while (pos_ < src_.size() &&
                           std::isxdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
                        const char hc = src_[pos_++];
                        std::uint64_t digit = 0;
                        if (hc >= '0' && hc <= '9') digit = static_cast<std::uint64_t>(hc - '0');
                        else if (hc >= 'a' && hc <= 'f') digit = static_cast<std::uint64_t>(hc - 'a' + 10);
                        else if (hc >= 'A' && hc <= 'F') digit = static_cast<std::uint64_t>(hc - 'A' + 10);
                        chVal = (chVal << 4u) | digit;
                    }
                    break;
                }
                default:
                    if (esc >= '0' && esc <= '7') {
                        chVal = static_cast<std::uint64_t>(esc - '0');
                        for (int oi = 0; oi < 2 && pos_ < src_.size() &&
                                         src_[pos_] >= '0' && src_[pos_] <= '7'; ++oi) {
                            chVal = (chVal << 3u) | static_cast<std::uint64_t>(src_[pos_++] - '0');
                        }
                    } else {
                        chVal = static_cast<unsigned char>(esc);
                    }
                    break;
                }
            } else {
                chVal = static_cast<unsigned char>(src_[pos_++]);
            }
        } else {
            markInvalid();
        }
        if (pos_ < src_.size() && src_[pos_] == '\'') {
            ++pos_;
        } else {
            markInvalid();
        }
        return EvalVal{chVal, false};
    }

    EvalVal parsePrimary() {
        if (!stepGuard()) return {};
        skipWs();
        if (pos_ >= src_.size()) {
            markInvalid();
            return {};
        }
        if (src_[pos_] == '(') {
            ++pos_;
            const auto v = parseTernary();
            if (!match(")")) markInvalid();
            return v;
        }
        if (src_[pos_] == '\'') {
            ++pos_;
            return parseCharLiteralAfterQuote();
        }
        if (std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
            const std::size_t start = pos_;
            int base = 10;
            if (src_[pos_] == '0' && pos_ + 1 < src_.size()) {
                if (src_[pos_ + 1] == 'x' || src_[pos_ + 1] == 'X') {
                    base = 16;
                    pos_ += 2;
                } else if (src_[pos_ + 1] == 'b' || src_[pos_ + 1] == 'B') {
                    base = 2;
                    pos_ += 2;
                } else if (std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])) != 0) {
                    base = 8;
                    ++pos_;
                }
            }
            std::string digits;
            while (pos_ < src_.size()) {
                const char c = src_[pos_];
                if (c == '\'') { ++pos_; continue; }
                const bool okDigit =
                    (base == 2 && (c == '0' || c == '1')) ||
                    (base == 8 && c >= '0' && c <= '7') ||
                    (base == 10 && std::isdigit(static_cast<unsigned char>(c)) != 0) ||
                    (base == 16 && std::isxdigit(static_cast<unsigned char>(c)) != 0);
                if (!okDigit) break;
                digits.push_back(c);
                ++pos_;
            }
            bool hasUnsignedSuffix = false;
            while (pos_ < src_.size() &&
                   (src_[pos_] == 'u' || src_[pos_] == 'U' ||
                    src_[pos_] == 'l' || src_[pos_] == 'L')) {
                if (src_[pos_] == 'u' || src_[pos_] == 'U') {
                    hasUnsignedSuffix = true;
                }
                ++pos_;
            }
            if (digits.empty() && src_[start] == '0') {
                return EvalVal{0ull, hasUnsignedSuffix};
            }
            if (digits.empty()) {
                markInvalid();
                return {};
            }
            const std::uint64_t parsed = std::strtoull(digits.c_str(), nullptr, base);
            const bool isUns = hasUnsignedSuffix ||
                (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
            return EvalVal{parsed, isUns};
        }
        if (isIdentStart(src_[pos_])) {
            std::string id = readIdentifier();
            if ((id == "L" || id == "u" || id == "U" || id == "u8") &&
                pos_ < src_.size() && src_[pos_] == '\'') {
                ++pos_;
                return parseCharLiteralAfterQuote();
            }
            std::string fullScoped = id;
            while (pos_ + 1 < src_.size() && src_[pos_] == ':' && src_[pos_ + 1] == ':') {
                pos_ += 2;
                const std::string rhs = readIdentifier();
                if (!rhs.empty()) {
                    fullScoped += "::" + rhs;
                    id = rhs;
                }
            }
            if (id == "true") return EvalVal{1ull, false};
            if (id == "false") return EvalVal{0ull, false};
            if (id == "defined") {
                skipWs();
                bool hasParen = false;
                if (pos_ < src_.size() && src_[pos_] == '(') {
                    hasParen = true;
                    ++pos_;
                }
                const std::string macroName = readIdentifier();
                if (hasParen) (void)match(")");
                if (macroName.empty()) {
                    markInvalid();
                    return {};
                }
                return EvalVal{
                    (macros_ && macros_->find(macroName) != macros_->end()) ? 1ull : 0ull,
                    false};
            }

            if (suppressDepth_ > 0) {
                skipWs();
                if (pos_ < src_.size() && src_[pos_] == '(') {
                    skipBalancedParenInSrc();
                }
                return {};
            }

            if (enumValues_) {
                if (const auto fit = enumValues_->find(fullScoped); fit != enumValues_->end()) {
                    return EvalVal{static_cast<std::uint64_t>(fit->second), false};
                }
                if (const auto eit = enumValues_->find(id); eit != enumValues_->end()) {
                    return EvalVal{static_cast<std::uint64_t>(eit->second), false};
                }
            }
            if (macros_ && depth_ < 16) {
                const auto mit = macros_->find(id);
                if (mit != macros_->end() && !mit->second.isFunctionLike) {
                    if (mit->second.body.empty()) return EvalVal{1ull, false};
                    CppConstExprEvaluator sub(mit->second.body, macros_, enumValues_);
                    sub.depth_ = depth_ + 1;
                    sub.steps_ = steps_;
                    const auto v = sub.evaluate();
                    steps_ = sub.steps_;
                    return EvalVal{static_cast<std::uint64_t>(v), false};
                }
            }
            // Swallow unknown preprocessor builtin calls like __has_feature(cxx_rtti)
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == '(') {
                if (macros_) {
                    skipBalancedParenInSrc();
                    return EvalVal{0ull, false};
                }
                markInvalid();
                return {};
            }
            if (enumValues_ && !macros_) {
                markInvalid();
            }
            return EvalVal{0ull, false};
        }
        if (suppressDepth_ > 0) {
            ++pos_;
            return {};
        }
        markInvalid();
        return {};
    }
};

inline std::string splicePhysicalLinesKeepNewlines(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    std::size_t deferredNewlines = 0;
    for (std::size_t i = 0; i < src.size();) {
        if (i == 0 || !isIdentCont(src[i - 1])) {
            const std::size_t rawEnd = tryScanAnyRawStringEnd(src, i);
            if (rawEnd != std::string_view::npos) {
                out.append(src.substr(i, rawEnd - i));
                i = rawEnd;
                continue;
            }
        }
        if (src[i] == '\\') {
            std::size_t look = i + 1;
            while (look < src.size() &&
                   (src[look] == ' ' || src[look] == '\t' || src[look] == '\r')) {
                ++look;
            }
            if (look < src.size() && src[look] == '\n') {
                ++deferredNewlines;
                i = look + 1;
                continue;
            }
        }
        if (src[i] == '\r') {
            ++i;
            continue;
        }
        if (src[i] == '\n') {
            out.append(deferredNewlines + 1, '\n');
            deferredNewlines = 0;
            ++i;
            continue;
        }
        out.push_back(src[i++]);
    }
    if (deferredNewlines > 0) {
        out.append(deferredNewlines, '\n');
    }
    return out;
}

inline std::string stripCommentsKeepLines(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size();) {
        if (i == 0 || !isIdentCont(src[i - 1])) {
            const std::size_t rawEnd = tryScanAnyRawStringEnd(src, i);
            if (rawEnd != std::string_view::npos) {
                out.append(src.substr(i, rawEnd - i));
                i = rawEnd;
                continue;
            }
        }
        if (src[i] == '"' || src[i] == '\'') {
            const char q = src[i];
            out.push_back(src[i++]);
            while (i < src.size()) {
                if (src[i] == '\\' && i + 1 < src.size()) {
                    out.push_back(src[i++]);
                    out.push_back(src[i++]);
                    continue;
                }
                const char c = src[i++];
                out.push_back(c);
                if (c == q) break;
            }
        } else if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
            i += 2;
            while (i < src.size() && src[i] != '\n') ++i;
        } else if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') out.push_back('\n');
                ++i;
            }
            if (i + 1 < src.size()) i += 2;
            out.push_back(' ');
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

inline std::string preprocessCppHeader(
    std::string_view headerContent,
    const std::unordered_map<std::string, std::string>& predefinedMacros) {
    const std::string spliced = splicePhysicalLinesKeepNewlines(headerContent);
    const std::string stripped = stripCommentsKeepLines(spliced);
    std::unordered_map<std::string, PpMacro> macros;
    macros.reserve(predefinedMacros.size() + 16);
    for (const auto& [k, v] : predefinedMacros) {
        PpMacro pm;
        pm.isFunctionLike = false;
        pm.body = v;
        macros[k] = std::move(pm);
    }

    struct PpBranch {
        bool active = true;
        bool parentActive = true;
        bool taken = false;
    };
    std::vector<PpBranch> stack;
    const auto isActive = [&]() noexcept -> bool {
        return stack.empty() || stack.back().active;
    };

    std::size_t globalExpansionBudget = 65536;
    const auto evalPpCondition = [&](std::string_view rawExpr) -> bool {
        std::size_t condBudget = std::min<std::size_t>(globalExpansionBudget, 16384);
        const std::string expanded =
            expandMacrosBounded(rawExpr, macros, true, condBudget);
        globalExpansionBudget -= (std::min<std::size_t>(globalExpansionBudget, 16384) - condBudget);
        return CppConstExprEvaluator(expanded, &macros).evaluate() != 0;
    };

    std::string out;
    out.reserve(stripped.size());
    std::string activeChunk;

    const auto flushActiveChunk = [&]() {
        if (activeChunk.empty()) return;
        out += expandMacrosBounded(activeChunk, macros, false, globalExpansionBudget);
        activeChunk.clear();
    };

    std::size_t pos = 0;
    while (pos < stripped.size()) {
        std::size_t nl = stripped.find('\n', pos);
        const bool hadNewline = (nl != std::string::npos);
        if (!hadNewline) nl = stripped.size();
        std::string line = stripped.substr(pos, nl - pos);
        pos = nl + 1;
        const std::size_t newlines = hadNewline ? 1u : 0u;

        const std::string trimmed = trimView(line);
        if (!trimmed.empty() && trimmed.front() == '#') {
            flushActiveChunk();
            const std::string dir = trimView(std::string_view(trimmed).substr(1));
            if (dir.rfind("ifdef ", 0) == 0 || dir.rfind("ifdef\t", 0) == 0) {
                const std::string name = trimView(std::string_view(dir).substr(6));
                const bool pAct = isActive();
                const bool cond = macros.find(name) != macros.end();
                const bool act = pAct && cond;
                stack.push_back(PpBranch{act, pAct, cond});
            } else if (dir.rfind("ifndef ", 0) == 0 || dir.rfind("ifndef\t", 0) == 0) {
                const std::string name = trimView(std::string_view(dir).substr(7));
                const bool pAct = isActive();
                const bool cond = macros.find(name) == macros.end();
                const bool act = pAct && cond;
                stack.push_back(PpBranch{act, pAct, cond});
            } else if (dir.rfind("if ", 0) == 0 || dir.rfind("if(", 0) == 0 || dir.rfind("if\t", 0) == 0) {
                const std::string expr = trimView(std::string_view(dir).substr(2));
                const bool pAct = isActive();
                const bool cond = pAct ? evalPpCondition(expr) : false;
                stack.push_back(PpBranch{pAct && cond, pAct, cond});
            } else if (dir.rfind("elif ", 0) == 0 || dir.rfind("elif(", 0) == 0 || dir.rfind("elif\t", 0) == 0) {
                if (!stack.empty()) {
                    const std::string expr = trimView(std::string_view(dir).substr(4));
                    const bool canTake = stack.back().parentActive && !stack.back().taken;
                    const bool cond = canTake ? evalPpCondition(expr) : false;
                    stack.back().active = cond;
                    if (cond) stack.back().taken = true;
                }
            } else if (dir == "else" || dir.rfind("else ", 0) == 0 || dir.rfind("else\t", 0) == 0) {
                if (!stack.empty()) {
                    stack.back().active = stack.back().parentActive && !stack.back().taken;
                    stack.back().taken = true;
                }
            } else if (dir == "endif" || dir.rfind("endif ", 0) == 0 || dir.rfind("endif\t", 0) == 0) {
                if (!stack.empty()) stack.pop_back();
            } else if (isActive() && (dir.rfind("define ", 0) == 0 || dir.rfind("define\t", 0) == 0)) {
                const std::string rest = trimView(std::string_view(dir).substr(6));
                std::size_t i = 0;
                while (i < rest.size() && isIdentCont(rest[i])) ++i;
                const std::string macroName = rest.substr(0, i);
                if (!macroName.empty() && !isReservedExportMacro(macroName)) {
                    PpMacro pm;
                    if (i < rest.size() && rest[i] == '(') {
                        // Function-like macro: '(' is immediately adjacent to macroName!
                        pm.isFunctionLike = true;
                        ++i; // skip '('
                        std::string curParam;
                        int pDepth = 1;
                        while (i < rest.size() && pDepth > 0) {
                            if (rest[i] == '(') {
                                ++pDepth;
                                curParam.push_back(rest[i++]);
                            } else if (rest[i] == ')') {
                                --pDepth;
                                ++i;
                                if (pDepth == 0) break;
                                curParam.push_back(')');
                            } else if (rest[i] == ',' && pDepth == 1) {
                                const std::string pt = trimView(curParam);
                                if (pt == "...") pm.variadic = true;
                                else if (!pt.empty()) pm.params.push_back(pt);
                                curParam.clear();
                                ++i;
                            } else {
                                curParam.push_back(rest[i++]);
                            }
                        }
                        const std::string lastPt = trimView(curParam);
                        if (lastPt == "...") pm.variadic = true;
                        else if (!lastPt.empty()) pm.params.push_back(lastPt);
                        pm.body = trimView(std::string_view(rest).substr(i));
                    } else {
                        pm.isFunctionLike = false;
                        pm.body = trimView(std::string_view(rest).substr(i));
                    }
                    macros[macroName] = std::move(pm);
                }
            } else if (isActive() && (dir.rfind("undef ", 0) == 0 || dir.rfind("undef\t", 0) == 0)) {
                const std::string macroName = trimView(std::string_view(dir).substr(5));
                if (!isReservedExportMacro(macroName)) {
                    macros.erase(macroName);
                }
            }
            out.append(newlines, '\n');
            continue;
        }

        if (isActive()) {
            activeChunk += line;
            activeChunk.append(newlines, '\n');
        } else {
            out.append(newlines, '\n');
        }
    }
    flushActiveChunk();
    return out;
}

// ============================================================================
// 2. Single-File C++17 Token Stream & Recursive-Descent Declaration AST Parser
// ============================================================================
enum class TokKind : std::uint8_t {
    Eof = 0,
    Ident,
    Number,
    StringLit,
    CharLit,
    LBrace,      // {
    RBrace,      // }
    LParen,      // (
    RParen,      // )
    LBracket,    // [
    RBracket,    // ]
    LAngle,      // <
    RAngle,      // >
    Semi,        // ;
    Comma,       // ,
    Colon,       // :
    ColonColon,  // ::
    Arrow,       // ->
    Star,        // *
    Amp,         // &
    AmpAmp,      // &&
    Equal,       // =
    Tilde,       // ~
    Ellipsis,    // ...
    Other,
};

struct Tok {
    TokKind kind = TokKind::Eof;
    std::string text;
};

inline std::string unquoteString(std::string_view sv) {
    std::string s = trimView(sv);
    std::size_t p = 0;
    if (s.rfind("u8", 0) == 0) p = 2;
    else if (!s.empty() && (s[0] == 'L' || s[0] == 'u' || s[0] == 'U')) p = 1;
    if (p < s.size() && s[p] == 'R' && p + 1 < s.size() && s[p + 1] == '"' && s.back() == '"') {
        const std::size_t lp = s.find('(', p + 2);
        if (lp != std::string::npos) {
            const std::string delim = s.substr(p + 2, lp - (p + 2));
            const std::size_t suffixLen = 2 + delim.size();
            if (s.size() >= lp + 1 + suffixLen &&
                s.compare(s.size() - suffixLen, suffixLen, ")" + delim + "\"") == 0) {
                return s.substr(lp + 1, s.size() - (lp + 1) - suffixLen);
            }
        }
    }
    if (p > 0 && p < s.size() && (s[p] == '"' || s[p] == '\'')) {
        s.erase(0, p);
    }
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

inline std::vector<Tok> tokenizeCppDeclarations(std::string_view src) {
    std::vector<Tok> tokens;
    tokens.reserve(src.size() / 4 + 16);

    for (std::size_t i = 0; i < src.size();) {
        const char ch = src[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++i;
            continue;
        }
        // Strip C++11/17 [[...]] attributes cleanly
        if (ch == '[' && i + 1 < src.size() && src[i + 1] == '[') {
            i += 2;
            int depth = 1;
            while (i < src.size() && depth > 0) {
                if (i + 1 < src.size() && src[i] == '[' && src[i + 1] == '[') {
                    ++depth;
                    i += 2;
                } else if (i + 1 < src.size() && src[i] == ']' && src[i + 1] == ']') {
                    --depth;
                    i += 2;
                } else {
                    ++i;
                }
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            const char q = ch;
            const std::size_t start = i++;
            while (i < src.size()) {
                if (src[i] == '\\' && i + 1 < src.size()) {
                    i += 2;
                    continue;
                }
                if (src[i++] == q) break;
            }
            tokens.push_back(Tok{
                q == '"' ? TokKind::StringLit : TokKind::CharLit,
                std::string(src.substr(start, i - start)),
            });
            continue;
        }
        if (isIdentStart(ch)) {
            const std::size_t rawEnd = tryScanAnyRawStringEnd(src, i);
            if (rawEnd != std::string_view::npos) {
                tokens.push_back(Tok{
                    TokKind::StringLit,
                    std::string(src.substr(i, rawEnd - i)),
                });
                i = rawEnd;
                continue;
            }
            const std::size_t start = i++;
            while (i < src.size() && isIdentCont(src[i])) ++i;
            std::string ident(src.substr(start, i - start));
            if ((ident == "L" || ident == "u" || ident == "U" || ident == "u8") &&
                i < src.size() && (src[i] == '"' || src[i] == '\'')) {
                const char q = src[i++];
                while (i < src.size()) {
                    if (src[i] == '\\' && i + 1 < src.size()) {
                        i += 2;
                        continue;
                    }
                    if (src[i++] == q) break;
                }
                tokens.push_back(Tok{
                    q == '"' ? TokKind::StringLit : TokKind::CharLit,
                    std::string(src.substr(start, i - start)),
                });
                continue;
            }
            // Strip compiler-specific __declspec(...) / __attribute__((...)) / alignas(...)
            if (ident == "__declspec" || ident == "__attribute__" || ident == "alignas") {
                while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n')) ++i;
                if (i < src.size() && src[i] == '(') {
                    int pDepth = 0;
                    while (i < src.size()) {
                        if (src[i] == '(') ++pDepth;
                        else if (src[i] == ')') {
                            --pDepth;
                            if (pDepth == 0) { ++i; break; }
                        }
                        ++i;
                    }
                }
                continue;
            }
            tokens.push_back(Tok{TokKind::Ident, std::move(ident)});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            const std::size_t start = i++;
            while (i < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[i])) != 0 ||
                    src[i] == '.' || src[i] == '\'' || src[i] == '_')) {
                ++i;
            }
            tokens.push_back(Tok{TokKind::Number, std::string(src.substr(start, i - start))});
            continue;
        }
        if (ch == ':' && i + 1 < src.size() && src[i + 1] == ':') {
            tokens.push_back(Tok{TokKind::ColonColon, "::"});
            i += 2;
            continue;
        }
        if (ch == '-' && i + 1 < src.size() && src[i + 1] == '>') {
            tokens.push_back(Tok{TokKind::Arrow, "->"});
            i += 2;
            continue;
        }
        if (ch == '&' && i + 1 < src.size() && src[i + 1] == '&') {
            tokens.push_back(Tok{TokKind::AmpAmp, "&&"});
            i += 2;
            continue;
        }
        if (ch == '.' && i + 2 < src.size() && src[i + 1] == '.' && src[i + 2] == '.') {
            tokens.push_back(Tok{TokKind::Ellipsis, "..."});
            i += 3;
            continue;
        }
        switch (ch) {
        case '{': tokens.push_back(Tok{TokKind::LBrace, "{"}); break;
        case '}': tokens.push_back(Tok{TokKind::RBrace, "}"}); break;
        case '(': tokens.push_back(Tok{TokKind::LParen, "("}); break;
        case ')': tokens.push_back(Tok{TokKind::RParen, ")"}); break;
        case '[': tokens.push_back(Tok{TokKind::LBracket, "["}); break;
        case ']': tokens.push_back(Tok{TokKind::RBracket, "]"}); break;
        case '<': tokens.push_back(Tok{TokKind::LAngle, "<"}); break;
        case '>': tokens.push_back(Tok{TokKind::RAngle, ">"}); break;
        case ';': tokens.push_back(Tok{TokKind::Semi, ";"}); break;
        case ',': tokens.push_back(Tok{TokKind::Comma, ","}); break;
        case ':': tokens.push_back(Tok{TokKind::Colon, ":"}); break;
        case '*': tokens.push_back(Tok{TokKind::Star, "*"}); break;
        case '&': tokens.push_back(Tok{TokKind::Amp, "&"}); break;
        case '=': tokens.push_back(Tok{TokKind::Equal, "="}); break;
        case '~': tokens.push_back(Tok{TokKind::Tilde, "~"}); break;
        default: tokens.push_back(Tok{TokKind::Other, std::string(1, ch)}); break;
        }
        ++i;
    }
    tokens.push_back(Tok{TokKind::Eof, ""});
    return tokens;
}

struct ExportMacroCall {
    std::string name;
    std::vector<std::string> args;
};

struct ParsedParam {
    std::string rawCppType;
    std::string name;
    bool hasExplicitName = false;
};

struct ParsedCallable {
    bool exported = false;
    bool isConstructor = false;
    bool isStatic = false;
    bool isConst = false;
    bool isDeletedOrPureVirtual = false;
    bool nonDeterministic = false;
    bool hasExplicitModule = false;
    std::string capability;
    std::string moduleOverride;
    std::string ctorFactoryOverride;
    std::string nsPrefix;
    std::string name;
    std::string qualifiedName;
    std::string returnCppType;
    std::vector<ParsedParam> params;
};

struct ParsedField {
    bool exported = false;
    bool readOnly = false;
    bool hasArrayDeclarator = false;
    std::string name;
    std::string cppType;
};

struct ParsedProperty {
    std::string propName;
    std::string getterMethod;
    std::string setterMethod;
    std::string explicitCppType;
};

struct ParsedEnumMember {
    std::string name;
    std::string exprText;
    std::int64_t value = 0;
};

struct ParsedEnum {
    bool hasExplicitModule = false;
    std::string moduleName;
    std::string nsPrefix;
    std::string name;
    std::string qualifiedName;
    std::vector<ParsedEnumMember> members;
};

struct ParsedRecord {
    bool isEcsStruct = false;
    bool hasExplicitModule = false;
    std::string moduleName;
    std::string nsPrefix;
    std::string name;
    std::string qualifiedName;
    std::vector<std::string> baseClasses;
    std::vector<ParsedField> fields;
    std::vector<ParsedProperty> properties;
    std::vector<ParsedCallable> methods;
};

class CppDeclarationParser {
public:
    CppDeclarationParser(
        const std::vector<Tok>& tokens,
        std::string defaultModuleName)
        : tok_(tokens), defaultModule_(std::move(defaultModuleName)) {}

    void parseTranslationUnit() {
        while (!atEnd()) {
            if (cur().kind == TokKind::RBrace) {
                advance();
                continue;
            }
            const std::size_t before = pos_;
            parseTopLevelItem();
            if (pos_ == before && !atEnd()) advance();
        }
    }

    std::unordered_map<std::string, std::string> typeAliases;
    std::unordered_set<std::string> declaredQualifiedTypes;
    std::vector<ParsedEnum> enums;
    std::vector<ParsedRecord> records;
    std::vector<ParsedCallable> freeFunctions;

    static std::string formatTokensNormalized(const std::vector<Tok>& slice) {
        std::string out;
        for (std::size_t i = 0; i < slice.size(); ++i) {
            const auto& t = slice[i];
            if (i > 0) {
                const auto& prev = slice[i - 1];
                const bool noSpaceBefore =
                    t.kind == TokKind::ColonColon ||
                    t.kind == TokKind::Comma ||
                    t.kind == TokKind::RParen ||
                    t.kind == TokKind::RBracket ||
                    t.kind == TokKind::RAngle ||
                    (t.kind == TokKind::LAngle && prev.kind == TokKind::Ident) ||
                    (t.kind == TokKind::LParen && prev.kind == TokKind::Ident) ||
                    (t.kind == TokKind::Star && (prev.kind == TokKind::Ident || prev.kind == TokKind::RAngle || prev.kind == TokKind::Star)) ||
                    (t.kind == TokKind::Amp && (prev.kind == TokKind::Ident || prev.kind == TokKind::RAngle)) ||
                    (t.kind == TokKind::AmpAmp && (prev.kind == TokKind::Ident || prev.kind == TokKind::RAngle));
                const bool noSpaceAfterPrev =
                    prev.kind == TokKind::ColonColon ||
                    prev.kind == TokKind::LAngle ||
                    prev.kind == TokKind::LParen ||
                    prev.kind == TokKind::LBracket ||
                    prev.kind == TokKind::Tilde;
                if (!noSpaceBefore && !noSpaceAfterPrev) {
                    out.push_back(' ');
                }
            }
            out += t.text;
        }
        return trimView(out);
    }

private:
    const std::vector<Tok>& tok_;
    std::size_t pos_ = 0;
    std::string defaultModule_;
    std::vector<std::string> nsStack_;
    std::unordered_map<std::string, std::int64_t> knownEnumConstants_;

    [[nodiscard]] bool atEnd() const noexcept {
        return pos_ >= tok_.size() || tok_[pos_].kind == TokKind::Eof;
    }

    [[nodiscard]] const Tok& cur() const noexcept {
        return tok_[std::min(pos_, tok_.size() - 1)];
    }

    [[nodiscard]] const Tok& peek(std::size_t offset = 1) const noexcept {
        return tok_[std::min(pos_ + offset, tok_.size() - 1)];
    }

    const Tok& advance() noexcept {
        const auto& t = cur();
        if (!atEnd()) ++pos_;
        return t;
    }

    bool checkIdent(std::string_view text) const noexcept {
        return cur().kind == TokKind::Ident && cur().text == text;
    }

    [[nodiscard]] std::string currentNsPrefix() const {
        std::string out;
        for (const auto& ns : nsStack_) {
            if (!ns.empty()) out += ns + "::";
        }
        return out;
    }

    void recordDeclaredType(const std::string& shortName) {
        if (shortName.empty()) return;
        knownTypes_.insert(shortName);
        const auto ns = currentNsPrefix();
        if (!ns.empty()) {
            declaredQualifiedTypes.insert(ns + shortName);
        } else {
            declaredQualifiedTypes.insert(shortName);
        }
    }

    void skipBalanced(TokKind openKind, TokKind closeKind) {
        if (cur().kind != openKind) return;
        int depth = 0;
        while (!atEnd()) {
            if (cur().kind == openKind) ++depth;
            else if (cur().kind == closeKind) {
                --depth;
                advance();
                if (depth <= 0) return;
                continue;
            }
            advance();
        }
    }

    void skipToSemicolonOrEndOfBlock() {
        while (!atEnd()) {
            if (cur().kind == TokKind::Semi) {
                advance();
                return;
            }
            if (cur().kind == TokKind::LBrace) {
                skipBalanced(TokKind::LBrace, TokKind::RBrace);
                if (cur().kind == TokKind::Semi) advance();
                return;
            }
            if (cur().kind == TokKind::RBrace) {
                return;
            }
            advance();
        }
    }

    // Consume C++ `operator ...` tokens up to parameter list `(` without polluting template angleDepth!
    void consumeOperatorNameTokens(std::vector<Tok>& prefix) {
        prefix.push_back(advance()); // 'operator'
        if (atEnd()) return;
        // operator ()
        if (cur().kind == TokKind::LParen && peek().kind == TokKind::RParen) {
            prefix.push_back(advance());
            prefix.push_back(advance());
            return;
        }
        // operator []
        if (cur().kind == TokKind::LBracket && peek().kind == TokKind::RBracket) {
            prefix.push_back(advance());
            prefix.push_back(advance());
            return;
        }
        // operator new / delete (optional [])
        if (cur().kind == TokKind::Ident &&
            (cur().text == "new" || cur().text == "delete")) {
            prefix.push_back(advance());
            if (cur().kind == TokKind::LBracket && peek().kind == TokKind::RBracket) {
                prefix.push_back(advance());
                prefix.push_back(advance());
            }
            return;
        }
        // Symbolic operator (`<`, `<<`, `<=`, `<=>`, `>`, `>>`, `>=`, `->`, `->*`, `==`, `!=`, `+`, `-`, `*`, etc.)
        // vs Conversion operator (`operator bool()`, `operator ns::Foo<T>()`)
        if (cur().kind != TokKind::Ident && cur().kind != TokKind::ColonColon) {
            while (!atEnd() &&
                   cur().kind != TokKind::LParen &&
                   cur().kind != TokKind::Semi &&
                   cur().kind != TokKind::LBrace &&
                   cur().kind != TokKind::RBrace) {
                prefix.push_back(advance());
            }
            return;
        }
        // Conversion operator: consume target type tokens up to `(`
        int convAngle = 0;
        while (!atEnd()) {
            if (cur().kind == TokKind::LAngle) {
                ++convAngle;
                prefix.push_back(advance());
                continue;
            }
            if (cur().kind == TokKind::RAngle && convAngle > 0) {
                --convAngle;
                prefix.push_back(advance());
                continue;
            }
            if (convAngle == 0 &&
                (cur().kind == TokKind::LParen ||
                 cur().kind == TokKind::Semi ||
                 cur().kind == TokKind::LBrace ||
                 cur().kind == TokKind::RBrace)) {
                break;
            }
            prefix.push_back(advance());
        }
    }

    std::vector<ExportMacroCall> consumeExportMacros() {
        std::vector<ExportMacroCall> list;
        while (!atEnd() && cur().kind == TokKind::Ident && isReservedExportMacro(cur().text)) {
            ExportMacroCall call;
            call.name = advance().text;
            if (cur().kind == TokKind::LParen) {
                advance(); // (
                std::vector<Tok> argToks;
                int pDepth = 0;
                int aDepth = 0;
                while (!atEnd()) {
                    if (cur().kind == TokKind::LParen) {
                        ++pDepth;
                        argToks.push_back(advance());
                    } else if (cur().kind == TokKind::RParen) {
                        if (pDepth == 0) {
                            advance();
                            break;
                        }
                        --pDepth;
                        argToks.push_back(advance());
                    } else if (cur().kind == TokKind::LAngle && pDepth == 0) {
                        ++aDepth;
                        argToks.push_back(advance());
                    } else if (cur().kind == TokKind::RAngle && pDepth == 0 && aDepth > 0) {
                        --aDepth;
                        argToks.push_back(advance());
                    } else if (cur().kind == TokKind::Comma && pDepth == 0 && aDepth == 0) {
                        advance();
                        call.args.push_back(unquoteString(formatTokensNormalized(argToks)));
                        argToks.clear();
                    } else {
                        argToks.push_back(advance());
                    }
                }
                const std::string lastArg = unquoteString(formatTokensNormalized(argToks));
                if (!lastArg.empty() || !call.args.empty()) {
                    call.args.push_back(lastArg);
                }
            }
            if (cur().kind == TokKind::Semi && call.name == "RS_EXPORT_PROPERTY") {
                advance();
            }
            list.push_back(std::move(call));
        }
        return list;
    }

    void parseNamespace() {
        if (checkIdent("inline")) advance();
        if (!checkIdent("namespace")) return;
        advance(); // namespace

        if (cur().kind == TokKind::Ident && peek().kind == TokKind::Equal) {
            skipToSemicolonOrEndOfBlock();
            return;
        }

        std::vector<std::string> pushed;
        while (!atEnd() && cur().kind != TokKind::LBrace && cur().kind != TokKind::Semi) {
            if (checkIdent("inline")) {
                advance();
                continue;
            }
            if (cur().kind == TokKind::Ident) {
                pushed.push_back(advance().text);
            } else if (cur().kind == TokKind::ColonColon) {
                advance();
            } else {
                advance();
            }
        }
        if (cur().kind == TokKind::Semi) {
            advance();
            return;
        }
        if (cur().kind != TokKind::LBrace) return;
        advance(); // {
        if (pushed.empty()) pushed.emplace_back("");
        for (const auto& p : pushed) nsStack_.push_back(p);

        while (!atEnd() && cur().kind != TokKind::RBrace) {
            const std::size_t before = pos_;
            parseTopLevelItem();
            if (pos_ == before && !atEnd() && cur().kind != TokKind::RBrace) advance();
        }
        if (cur().kind == TokKind::RBrace) advance();
        for (std::size_t i = 0; i < pushed.size() && !nsStack_.empty(); ++i) {
            nsStack_.pop_back();
        }
    }

    void parseUsingOrTypedef() {
        if (checkIdent("using")) {
            advance();
            if (checkIdent("namespace")) {
                skipToSemicolonOrEndOfBlock();
                return;
            }
            if (cur().kind != TokKind::Ident) {
                skipToSemicolonOrEndOfBlock();
                return;
            }
            const std::string aliasName = advance().text;
            if (cur().kind != TokKind::Equal) {
                skipToSemicolonOrEndOfBlock();
                return;
            }
            advance(); // =
            std::vector<Tok> rhs;
            while (!atEnd() && cur().kind != TokKind::Semi && cur().kind != TokKind::RBrace) {
                rhs.push_back(advance());
            }
            if (cur().kind == TokKind::Semi) advance();
            const std::string target = formatTokensNormalized(rhs);
            if (!aliasName.empty() && !target.empty()) {
                recordDeclaredType(aliasName);
                typeAliases[aliasName] = target;
                const auto ns = currentNsPrefix();
                if (!ns.empty()) typeAliases[ns + aliasName] = target;
            }
            return;
        }
        if (checkIdent("typedef")) {
            advance();
            std::vector<Tok> body;
            while (!atEnd() && cur().kind != TokKind::Semi && cur().kind != TokKind::RBrace) {
                if (cur().kind == TokKind::LBrace) {
                    skipBalanced(TokKind::LBrace, TokKind::RBrace);
                    continue;
                }
                body.push_back(advance());
            }
            if (cur().kind == TokKind::Semi) advance();
            if (body.size() >= 2 && body.back().kind == TokKind::Ident) {
                const std::string aliasName = body.back().text;
                body.pop_back();
                bool hasParen = false;
                for (const auto& t : body) {
                    if (t.kind == TokKind::LParen || t.kind == TokKind::RParen) {
                        hasParen = true;
                        break;
                    }
                }
                const std::string target = formatTokensNormalized(body);
                if (!hasParen && !aliasName.empty() && !target.empty()) {
                    recordDeclaredType(aliasName);
                    typeAliases[aliasName] = target;
                    const auto ns = currentNsPrefix();
                    if (!ns.empty()) typeAliases[ns + aliasName] = target;
                }
            }
        }
    }

    void parseEnum(bool exported, std::string explicitModName = {}) {
        advance(); // enum
        if (checkIdent("class") || checkIdent("struct")) advance();
        std::string enumName;
        if (cur().kind == TokKind::Ident) {
            enumName = advance().text;
        }
        recordDeclaredType(enumName);
        while (!atEnd() && cur().kind != TokKind::LBrace && cur().kind != TokKind::Semi) {
            advance();
        }
        if (cur().kind == TokKind::Semi) {
            advance();
            return;
        }
        if (cur().kind != TokKind::LBrace) return;
        advance(); // {

        ParsedEnum pe;
        pe.hasExplicitModule = !explicitModName.empty();
        pe.moduleName = explicitModName.empty() ? defaultModule_ : std::move(explicitModName);
        pe.nsPrefix = currentNsPrefix();
        pe.name = enumName;
        pe.qualifiedName = pe.nsPrefix + enumName;
        std::int64_t nextVal = 0;

        while (!atEnd() && cur().kind != TokKind::RBrace) {
            (void)consumeExportMacros();
            if (cur().kind != TokKind::Ident) {
                advance();
                continue;
            }
            ParsedEnumMember mem;
            mem.name = advance().text;
            if (cur().kind == TokKind::Equal) {
                advance();
                std::vector<Tok> exprToks;
                int pDepth = 0;
                while (!atEnd() &&
                       !(pDepth == 0 && (cur().kind == TokKind::Comma || cur().kind == TokKind::RBrace))) {
                    if (cur().kind == TokKind::LParen) ++pDepth;
                    else if (cur().kind == TokKind::RParen && pDepth > 0) --pDepth;
                    exprToks.push_back(advance());
                }
                mem.exprText = formatTokensNormalized(exprToks);
                bool evalOk = false;
                const std::int64_t evaluated =
                    CppConstExprEvaluator(mem.exprText, nullptr, &knownEnumConstants_).evaluate(&evalOk);
                if (evalOk) {
                    mem.value = evaluated;
                    nextVal = static_cast<std::int64_t>(static_cast<std::uint64_t>(evaluated) + 1ull);
                    bool isSimpleDecimal = !mem.exprText.empty();
                    for (std::size_t ci = 0; ci < mem.exprText.size(); ++ci) {
                        const char c = mem.exprText[ci];
                        if (ci == 0 && (c == '-' || c == '+')) continue;
                        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                            isSimpleDecimal = false;
                            break;
                        }
                    }
                    if (!isSimpleDecimal) {
                        mem.exprText = std::to_string(evaluated);
                    }
                } else {
                    mem.value = nextVal++;
                    mem.exprText = std::to_string(mem.value);
                }
            } else {
                mem.value = nextVal++;
                mem.exprText = std::to_string(mem.value);
            }
            knownEnumConstants_[mem.name] = mem.value;
            if (!enumName.empty()) {
                knownEnumConstants_[enumName + "::" + mem.name] = mem.value;
            }
            pe.members.push_back(std::move(mem));
            if (cur().kind == TokKind::Comma) advance();
        }
        if (cur().kind == TokKind::RBrace) advance();
        if (cur().kind == TokKind::Semi) advance();

        if (exported && !pe.name.empty()) {
            enums.push_back(std::move(pe));
        }
    }

    static bool isQualifierWord(std::string_view w) noexcept {
        return w == "virtual" || w == "inline" || w == "constexpr" ||
            w == "consteval" || w == "static" || w == "explicit" ||
            w == "friend" || w == "extern";
    }

    static bool isBuiltinOrTypeWord(
        const std::string& token,
        const std::unordered_set<std::string>& knownTypes,
        const std::unordered_map<std::string, std::string>& aliases) {
        if (token.empty()) return true;
        if (token.back() == '*' || token.back() == '&' || token.back() == '>') return true;
        if (token.find("::") != std::string::npos) return true;
        static const std::unordered_set<std::string> builtins = {
            "int", "long", "short", "char", "float", "double", "bool", "void",
            "unsigned", "signed", "const", "volatile", "wchar_t", "char8_t",
            "char16_t", "char32_t", "int8_t", "uint8_t", "int16_t", "uint16_t",
            "int32_t", "uint32_t", "int64_t", "uint64_t", "size_t", "ssize_t",
            "ptrdiff_t", "intptr_t", "uintptr_t", "Vec2", "Vec3", "Quat",
            "Vec2f", "Vec3f", "Quatf", "Fixed", "Fixed16", "Fixed64", "string",
        };
        if (builtins.find(token) != builtins.end()) return true;
        if (knownTypes.find(token) != knownTypes.end()) return true;
        if (aliases.find(token) != aliases.end()) return true;
        return false;
    }

    static bool hasCoreTypeToken(const std::vector<Tok>& toks, std::size_t count) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            if (toks[i].kind != TokKind::Ident) continue;
            const std::string& t = toks[i].text;
            if (t == "const" || t == "volatile" || t == "mutable" ||
                t == "struct" || t == "class" || t == "enum" || t == "typename") {
                continue;
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] bool looksLikeTemplateArgListAtPos() const noexcept {
        if (cur().kind != TokKind::LAngle) return false;
        if (pos_ == 0 || tok_[pos_ - 1].kind != TokKind::Ident) return false;
        int lp = 0;
        int lb = 0;
        int ls = 0;
        int la = 1;
        for (std::size_t scan = pos_ + 1; scan < tok_.size(); ++scan) {
            const auto sk = tok_[scan].kind;
            if (sk == TokKind::Eof) return false;
            if (sk == TokKind::LParen) { ++lp; continue; }
            if (sk == TokKind::RParen) {
                if (lp == 0) return false;
                --lp;
                continue;
            }
            if (sk == TokKind::LBrace) { ++lb; continue; }
            if (sk == TokKind::RBrace) {
                if (lb == 0) return false;
                --lb;
                continue;
            }
            if (sk == TokKind::LBracket) { ++ls; continue; }
            if (sk == TokKind::RBracket) {
                if (ls == 0) return false;
                --ls;
                continue;
            }
            if (lp == 0 && lb == 0 && ls == 0) {
                if (sk == TokKind::Semi || sk == TokKind::Equal) return false;
                if (sk == TokKind::LAngle) {
                    ++la;
                } else if (sk == TokKind::RAngle) {
                    --la;
                    if (la == 0) return true;
                }
            }
        }
        return false;
    }

    std::vector<ParsedParam> parseParamList(
        const std::unordered_set<std::string>& knownTypes) {
        std::vector<ParsedParam> out;
        if (cur().kind != TokKind::LParen) return out;
        advance(); // (
        if (cur().kind == TokKind::RParen) {
            advance();
            return out;
        }
        int paramIdx = 0;
        while (!atEnd() && cur().kind != TokKind::RParen) {
            std::vector<Tok> pToks;
            int pDepth = 0;
            int aDepth = 0;
            int bDepth = 0;
            int sDepth = 0;
            bool seenEqual = false;
            while (!atEnd()) {
                const auto k = cur().kind;
                if (pDepth == 0 && aDepth == 0 && bDepth == 0 && sDepth == 0) {
                    if (k == TokKind::Comma || k == TokKind::RParen) break;
                    if (k == TokKind::Equal) {
                        seenEqual = true;
                        advance();
                        continue;
                    }
                }
                if (k == TokKind::LParen) ++pDepth;
                else if (k == TokKind::RParen && pDepth > 0) --pDepth;
                else if (k == TokKind::LAngle && pDepth == 0 && bDepth == 0 && sDepth == 0) {
                    if (!seenEqual || aDepth > 0 || looksLikeTemplateArgListAtPos()) {
                        ++aDepth;
                    }
                } else if (k == TokKind::RAngle && pDepth == 0 && bDepth == 0 && sDepth == 0 && aDepth > 0) {
                    --aDepth;
                }
                else if (k == TokKind::LBrace) ++bDepth;
                else if (k == TokKind::RBrace && bDepth > 0) --bDepth;
                else if (k == TokKind::LBracket) ++sDepth;
                else if (k == TokKind::RBracket && sDepth > 0) --sDepth;

                if (!seenEqual) {
                    pToks.push_back(advance());
                } else {
                    advance();
                }
            }
            if (cur().kind == TokKind::Comma) advance();

            if (pToks.size() == 1 && pToks[0].kind == TokKind::Ident && pToks[0].text == "void") {
                continue;
            }
            if (!pToks.empty()) {
                ParsedParam param;
                if (pToks.size() >= 2 &&
                    pToks.back().kind == TokKind::Ident &&
                    pToks[pToks.size() - 2].kind != TokKind::ColonColon &&
                    hasCoreTypeToken(pToks, pToks.size() - 1) &&
                    !isBuiltinOrTypeWord(pToks.back().text, knownTypes, typeAliases)) {
                    param.name = pToks.back().text;
                    param.hasExplicitName = true;
                    pToks.pop_back();
                    param.rawCppType = formatTokensNormalized(pToks);
                } else {
                    param.rawCppType = formatTokensNormalized(pToks);
                    param.name = "arg" + std::to_string(paramIdx);
                    param.hasExplicitName = false;
                }
                out.push_back(std::move(param));
                ++paramIdx;
            }
        }
        if (cur().kind == TokKind::RParen) advance();
        return out;
    }

    void parseRecord(bool exportedClass, bool exportedStruct, std::string explicitModuleName) {
        const std::string kw = advance().text;
        (void)kw;
        std::vector<std::string> headIdents;
        while (!atEnd() &&
               cur().kind != TokKind::Colon &&
               cur().kind != TokKind::LBrace &&
               cur().kind != TokKind::Semi) {
            if (cur().kind == TokKind::Ident && cur().text != "final") {
                headIdents.push_back(cur().text);
            }
            if (cur().kind == TokKind::LAngle) {
                skipBalanced(TokKind::LAngle, TokKind::RAngle);
                continue;
            }
            advance();
        }
        const std::string recShortName = headIdents.empty() ? std::string{} : headIdents.back();
        recordDeclaredType(recShortName);

        if (cur().kind == TokKind::Semi) {
            advance(); // forward declaration
            return;
        }
        std::vector<std::string> bases;
        if (cur().kind == TokKind::Colon) {
            advance(); // :
            std::vector<Tok> baseToks;
            int baseAngle = 0;
            int baseParen = 0;
            while (!atEnd() &&
                   !(baseAngle == 0 && baseParen == 0 &&
                     (cur().kind == TokKind::LBrace || cur().kind == TokKind::Semi))) {
                const auto bk = cur().kind;
                if (baseAngle == 0 && baseParen == 0 && bk == TokKind::Comma) {
                    advance();
                    const std::string b = formatTokensNormalized(baseToks);
                    if (!b.empty()) bases.push_back(b);
                    baseToks.clear();
                    continue;
                }
                if (baseAngle == 0 && baseParen == 0 && bk == TokKind::Ident &&
                    (cur().text == "public" || cur().text == "protected" ||
                     cur().text == "private" || cur().text == "virtual")) {
                    advance();
                    continue;
                }
                if (bk == TokKind::LParen && baseAngle > 0) ++baseParen;
                else if (bk == TokKind::RParen && baseParen > 0) --baseParen;
                else if (bk == TokKind::LAngle && baseParen == 0) ++baseAngle;
                else if (bk == TokKind::RAngle && baseParen == 0 && baseAngle > 0) --baseAngle;
                baseToks.push_back(advance());
            }
            const std::string lastBase = formatTokensNormalized(baseToks);
            if (!lastBase.empty()) bases.push_back(lastBase);
        }
        if (cur().kind != TokKind::LBrace) {
            skipToSemicolonOrEndOfBlock();
            return;
        }
        if (!exportedClass && !exportedStruct) {
            skipBalanced(TokKind::LBrace, TokKind::RBrace);
            if (cur().kind == TokKind::Semi) advance();
            return;
        }

        advance(); // {
        ParsedRecord rec;
        rec.isEcsStruct = exportedStruct;
        rec.hasExplicitModule = !explicitModuleName.empty();
        rec.moduleName = explicitModuleName.empty() ? defaultModule_ : std::move(explicitModuleName);
        rec.nsPrefix = currentNsPrefix();
        rec.name = recShortName;
        rec.qualifiedName = rec.nsPrefix + rec.name;
        rec.baseClasses = std::move(bases);

        nsStack_.push_back(rec.name);

        while (!atEnd() && cur().kind != TokKind::RBrace) {
            auto macros = consumeExportMacros();
            bool consumedOnlyProps = !macros.empty();
            for (const auto& m : macros) {
                if (m.name == "RS_EXPORT_PROPERTY" && m.args.size() >= 2) {
                    ParsedProperty prop;
                    prop.propName = m.args[0];
                    prop.getterMethod = m.args[1];
                    if (m.args.size() >= 3) prop.setterMethod = m.args[2];
                    if (m.args.size() >= 4) prop.explicitCppType = m.args[3];
                    rec.properties.push_back(std::move(prop));
                } else {
                    consumedOnlyProps = false;
                }
            }
            if (consumedOnlyProps && !macros.empty()) {
                continue;
            }

            if (cur().kind == TokKind::Ident &&
                (cur().text == "public" || cur().text == "private" || cur().text == "protected") &&
                peek().kind == TokKind::Colon) {
                advance();
                advance();
                continue;
            }

            if (checkIdent("using") || checkIdent("typedef")) {
                parseUsingOrTypedef();
                continue;
            }
            if (checkIdent("template")) {
                advance();
                if (cur().kind == TokKind::LAngle) skipBalanced(TokKind::LAngle, TokKind::RAngle);
                skipToSemicolonOrEndOfBlock();
                continue;
            }
            if (checkIdent("enum")) {
                bool enumExp = false;
                std::string enumMod;
                for (const auto& m : macros) {
                    if (m.name == "RS_EXPORT_ENUM") {
                        enumExp = true;
                        if (!m.args.empty() && !m.args[0].empty()) enumMod = m.args[0];
                    }
                }
                parseEnum(enumExp, std::move(enumMod));
                continue;
            }
            if ((checkIdent("class") || checkIdent("struct")) &&
                (peek().kind == TokKind::Ident || peek().kind == TokKind::LBrace)) {
                std::size_t scan = pos_ + 1;
                while (scan < tok_.size() &&
                       tok_[scan].kind != TokKind::LBrace &&
                       tok_[scan].kind != TokKind::Semi) {
                    ++scan;
                }
                if (scan < tok_.size() && tok_[scan].kind == TokKind::LBrace) {
                    parseRecord(false, false, {});
                    continue;
                }
            }
            if (cur().kind == TokKind::Semi) {
                advance();
                continue;
            }
            const std::size_t beforeMember = pos_;
            parseClassMember(rec, macros);
            if (pos_ == beforeMember && !atEnd() && cur().kind != TokKind::RBrace) advance();
        }

        if (!nsStack_.empty()) nsStack_.pop_back();
        if (cur().kind == TokKind::RBrace) advance();
        if (cur().kind == TokKind::Semi) advance();

        if (!rec.name.empty()) {
            records.push_back(std::move(rec));
        }
    }

    void skipFieldDeclaratorInitializerAndBitfield(bool& outHasArrayDeclarator) {
        outHasArrayDeclarator = false;
        while (!atEnd() && cur().kind == TokKind::LBracket) {
            outHasArrayDeclarator = true;
            skipBalanced(TokKind::LBracket, TokKind::RBracket);
        }
        if (cur().kind == TokKind::Colon) {
            // Bitfield width `: 4`
            advance();
            int pDepth = 0;
            while (!atEnd() &&
                   !(pDepth == 0 && (cur().kind == TokKind::Comma ||
                                     cur().kind == TokKind::Semi ||
                                     cur().kind == TokKind::RBrace))) {
                if (cur().kind == TokKind::LParen) ++pDepth;
                else if (cur().kind == TokKind::RParen && pDepth > 0) --pDepth;
                advance();
            }
            return;
        }
        if (cur().kind == TokKind::Equal) {
            advance(); // =
            int pDepth = 0;
            int bDepth = 0;
            int sDepth = 0;
            int aDepth = 0;
            while (!atEnd()) {
                const auto k = cur().kind;
                if (pDepth == 0 && bDepth == 0 && sDepth == 0 && aDepth == 0 &&
                    (k == TokKind::Comma || k == TokKind::Semi || k == TokKind::RBrace)) {
                    break;
                }
                if (k == TokKind::LParen) ++pDepth;
                else if (k == TokKind::RParen && pDepth > 0) --pDepth;
                else if (k == TokKind::LBrace) ++bDepth;
                else if (k == TokKind::RBrace && bDepth > 0) --bDepth;
                else if (k == TokKind::LBracket) ++sDepth;
                else if (k == TokKind::RBracket && sDepth > 0) --sDepth;
                else if (k == TokKind::LAngle && pDepth == 0 && bDepth == 0 && sDepth == 0) {
                    if (aDepth > 0 || looksLikeTemplateArgListAtPos()) ++aDepth;
                } else if (k == TokKind::RAngle && pDepth == 0 && bDepth == 0 && sDepth == 0 && aDepth > 0) {
                    --aDepth;
                }
                advance();
            }
        } else if (cur().kind == TokKind::LBrace) {
            skipBalanced(TokKind::LBrace, TokKind::RBrace);
        }
    }

    void parseClassMember(ParsedRecord& rec, const std::vector<ExportMacroCall>& macros) {
        bool hasFieldMacro = false;
        bool hasCtorMacro = false;
        bool hasMethodMacro = false;
        bool nonDet = false;
        std::string cap;
        std::string ctorOverride;

        for (const auto& m : macros) {
            if (m.name == "RS_EXPORT_FIELD") {
                hasFieldMacro = true;
            } else if (m.name == "RS_EXPORT_CONSTRUCTOR") {
                hasCtorMacro = true;
                if (!m.args.empty()) ctorOverride = m.args[0];
            } else if (m.name == "RS_EXPORT_METHOD" || m.name == "RS_EXPORT_METHOD_EX") {
                hasMethodMacro = true;
                for (const auto& a : m.args) {
                    if (a == "NonDeterministic") nonDet = true;
                    else if (a != "Deterministic" && !a.empty()) cap = a;
                }
            }
        }

        // Destructor `~ClassName(...)`
        if (cur().kind == TokKind::Tilde ||
            (checkIdent("virtual") && peek().kind == TokKind::Tilde)) {
            skipToSemicolonOrEndOfBlock();
            return;
        }

        // Scan prefix tokens up to `(`, `,`, `:`, `;`, `=`, `[` or `{` (outside `<...>`)
        std::vector<Tok> prefix;
        int angleDepth = 0;
        int parenDepth = 0;
        bool isOperatorDecl = false;
        while (!atEnd()) {
            const auto k = cur().kind;
            if (angleDepth == 0 && parenDepth == 0 && k == TokKind::Ident && cur().text == "operator") {
                isOperatorDecl = true;
                consumeOperatorNameTokens(prefix);
                break;
            }
            if (angleDepth > 0 && k == TokKind::LParen) {
                ++parenDepth;
                prefix.push_back(advance());
                continue;
            }
            if (angleDepth > 0 && k == TokKind::RParen && parenDepth > 0) {
                --parenDepth;
                prefix.push_back(advance());
                continue;
            }
            if (parenDepth == 0 && k == TokKind::LAngle) {
                ++angleDepth;
                prefix.push_back(advance());
                continue;
            }
            if (parenDepth == 0 && k == TokKind::RAngle && angleDepth > 0) {
                --angleDepth;
                prefix.push_back(advance());
                continue;
            }
            if (angleDepth == 0 && parenDepth == 0 &&
                (k == TokKind::LParen || k == TokKind::Semi ||
                 k == TokKind::Comma || k == TokKind::Colon ||
                 k == TokKind::LBracket ||
                 k == TokKind::Equal || k == TokKind::LBrace ||
                 k == TokKind::RBrace)) {
                break;
            }
            prefix.push_back(advance());
        }

        if (prefix.empty()) {
            if (!atEnd() && cur().kind != TokKind::RBrace) advance();
            return;
        }

        // Method, Operator, or Constructor declaration: hit `(`
        if (cur().kind == TokKind::LParen) {
            ParsedCallable fn;
            fn.nsPrefix = rec.nsPrefix;
            fn.params = parseParamList(knownTypes_);
            fn.nonDeterministic = nonDet;
            fn.capability = cap;
            fn.ctorFactoryOverride = ctorOverride;

            std::vector<Tok> trailingRet;
            bool sawArrow = false;
            while (!atEnd() &&
                   cur().kind != TokKind::Semi &&
                   cur().kind != TokKind::LBrace &&
                   cur().kind != TokKind::Colon &&
                   cur().kind != TokKind::RBrace) {
                if (cur().kind == TokKind::Arrow) {
                    sawArrow = true;
                    advance();
                    continue;
                }
                if (sawArrow) {
                    if (cur().kind == TokKind::Ident &&
                        (cur().text == "override" || cur().text == "final" || cur().text == "noexcept")) {
                        sawArrow = false;
                    } else if (cur().kind == TokKind::Equal) {
                        sawArrow = false;
                    } else {
                        trailingRet.push_back(advance());
                        continue;
                    }
                }
                if (checkIdent("const")) {
                    fn.isConst = true;
                    advance();
                } else if (checkIdent("noexcept")) {
                    advance();
                    if (cur().kind == TokKind::LParen) skipBalanced(TokKind::LParen, TokKind::RParen);
                } else if (cur().kind == TokKind::Equal) {
                    advance();
                    if (checkIdent("delete") || (cur().kind == TokKind::Number && cur().text == "0")) {
                        fn.isDeletedOrPureVirtual = true;
                    }
                    if (!atEnd() && cur().kind != TokKind::Semi) advance();
                } else {
                    advance();
                }
            }

            if (cur().kind == TokKind::Colon) {
                advance();
                while (!atEnd() && cur().kind != TokKind::LBrace && cur().kind != TokKind::Semi) {
                    bool sawTargetName = false;
                    while (!atEnd() &&
                           (cur().kind == TokKind::Ident ||
                            cur().kind == TokKind::ColonColon ||
                            cur().kind == TokKind::LAngle)) {
                        sawTargetName = true;
                        if (cur().kind == TokKind::LAngle) skipBalanced(TokKind::LAngle, TokKind::RAngle);
                        else advance();
                    }
                    if (!sawTargetName) break;
                    if (cur().kind == TokKind::LParen) {
                        skipBalanced(TokKind::LParen, TokKind::RParen);
                    } else if (cur().kind == TokKind::LBrace) {
                        skipBalanced(TokKind::LBrace, TokKind::RBrace);
                    }
                    if (cur().kind == TokKind::Ellipsis) advance();
                    if (cur().kind == TokKind::Comma) {
                        advance();
                    } else {
                        break;
                    }
                }
            }
            if (cur().kind == TokKind::LBrace) {
                skipBalanced(TokKind::LBrace, TokKind::RBrace);
                if (cur().kind == TokKind::Semi) advance();
            } else if (cur().kind == TokKind::Semi) {
                advance();
            }

            if (isOperatorDecl) {
                return;
            }

            bool isCtorSyntax = false;
            std::vector<Tok> filteredPrefix;
            for (const auto& pt : prefix) {
                if (pt.kind == TokKind::Ident && pt.text == "explicit") continue;
                filteredPrefix.push_back(pt);
            }
            if (filteredPrefix.size() == 1 &&
                filteredPrefix[0].kind == TokKind::Ident &&
                filteredPrefix[0].text == rec.name) {
                isCtorSyntax = true;
            }

            if (hasCtorMacro || isCtorSyntax) {
                fn.isConstructor = true;
                fn.exported = hasCtorMacro && !rec.isEcsStruct && !fn.isDeletedOrPureVirtual;
                fn.isStatic = true;
                fn.name = ctorOverride.empty() ? std::string("Create") : ctorOverride;
                fn.returnCppType = rec.name;
            } else {
                if (filteredPrefix.empty() || filteredPrefix.back().kind != TokKind::Ident) {
                    return;
                }
                fn.name = filteredPrefix.back().text;
                filteredPrefix.pop_back();
                std::vector<Tok> retToks;
                for (const auto& pt : filteredPrefix) {
                    if (pt.kind == TokKind::Ident && pt.text == "static") {
                        fn.isStatic = true;
                        continue;
                    }
                    if (pt.kind == TokKind::Ident && isQualifierWord(pt.text)) {
                        continue;
                    }
                    retToks.push_back(pt);
                }
                if (!trailingRet.empty()) {
                    fn.returnCppType = formatTokensNormalized(trailingRet);
                } else {
                    fn.returnCppType = formatTokensNormalized(retToks);
                }
                fn.exported = hasMethodMacro && !rec.isEcsStruct && !fn.isDeletedOrPureVirtual;
            }
            rec.methods.push_back(std::move(fn));
            return;
        }

        // Field declaration (single or comma-separated multi-declarator `int x, y;`)
        if (prefix.size() < 2 || prefix.back().kind != TokKind::Ident) {
            skipToSemicolonOrEndOfBlock();
            return;
        }

        const std::string firstFieldName = prefix.back().text;
        prefix.pop_back();

        // Separate trailing pointer/reference declarator modifiers (`int *x, y;`) from shared base type
        std::vector<Tok> firstDeclPtrToks;
        while (!prefix.empty() &&
               (prefix.back().kind == TokKind::Star ||
                prefix.back().kind == TokKind::Amp ||
                prefix.back().kind == TokKind::AmpAmp)) {
            firstDeclPtrToks.insert(firstDeclPtrToks.begin(), prefix.back());
            prefix.pop_back();
        }

        bool isStaticOrFriend = false;
        bool isConstField = false;
        std::vector<Tok> baseTypeToks;
        for (const auto& pt : prefix) {
            if (pt.kind == TokKind::Ident &&
                (pt.text == "static" || pt.text == "friend" || pt.text == "constexpr")) {
                isStaticOrFriend = true;
                if (pt.text == "constexpr") isConstField = true;
            }
            if (pt.kind == TokKind::Ident && pt.text == "const") {
                isConstField = true;
            }
            if (pt.kind == TokKind::Ident && pt.text == "mutable") {
                continue;
            }
            baseTypeToks.push_back(pt);
        }

        const auto emitFieldIfExported = [&](
            const std::string& fName,
            const std::vector<Tok>& declPtrToks,
            bool hasArr) {
            if (fName.empty() || baseTypeToks.empty()) return;
            std::vector<Tok> fullType = baseTypeToks;
            fullType.insert(fullType.end(), declPtrToks.begin(), declPtrToks.end());
            if (rec.isEcsStruct) {
                if (!isStaticOrFriend) {
                    ParsedField f;
                    f.exported = true;
                    f.readOnly = false;
                    f.hasArrayDeclarator = hasArr;
                    f.name = fName;
                    f.cppType = formatTokensNormalized(fullType);
                    rec.fields.push_back(std::move(f));
                }
            } else if (hasFieldMacro && !isStaticOrFriend) {
                ParsedField f;
                f.exported = true;
                f.readOnly = isConstField;
                f.hasArrayDeclarator = hasArr;
                f.name = fName;
                f.cppType = formatTokensNormalized(fullType);
                rec.fields.push_back(std::move(f));
            }
        };

        bool firstHasArr = false;
        skipFieldDeclaratorInitializerAndBitfield(firstHasArr);
        emitFieldIfExported(firstFieldName, firstDeclPtrToks, firstHasArr);

        while (!atEnd() && cur().kind == TokKind::Comma) {
            advance(); // ,
            std::vector<Tok> nextPtrToks;
            while (!atEnd() &&
                   (cur().kind == TokKind::Star ||
                    cur().kind == TokKind::Amp ||
                    cur().kind == TokKind::AmpAmp ||
                    (cur().kind == TokKind::Ident && cur().text == "const"))) {
                nextPtrToks.push_back(advance());
            }
            if (cur().kind != TokKind::Ident) {
                skipToSemicolonOrEndOfBlock();
                return;
            }
            const std::string nextName = advance().text;
            bool nextHasArr = false;
            skipFieldDeclaratorInitializerAndBitfield(nextHasArr);
            emitFieldIfExported(nextName, nextPtrToks, nextHasArr);
        }

        if (cur().kind == TokKind::Semi) {
            advance();
        } else {
            skipToSemicolonOrEndOfBlock();
        }
    }

    void parseTopLevelFunction(const std::vector<ExportMacroCall>& macros) {
        bool exported = false;
        bool nonDet = false;
        bool hasExplicitMod = false;
        std::string cap;
        std::string funcModule = defaultModule_;
        for (const auto& m : macros) {
            if (m.name == "RS_EXPORT_FUNCTION") {
                exported = true;
                for (std::size_t i = 0; i < m.args.size(); ++i) {
                    if (m.args[i] == "NonDeterministic") {
                        nonDet = true;
                    } else if (m.args[i] == "Deterministic") {
                        continue;
                    } else if (i == 0 && !m.args[i].empty()) {
                        funcModule = m.args[i];
                        hasExplicitMod = true;
                    } else if (!m.args[i].empty()) {
                        cap = m.args[i];
                    }
                }
            } else if (m.name == "RS_EXPORT_FUNCTION_EX") {
                exported = true;
                for (std::size_t i = 0; i < m.args.size(); ++i) {
                    if (m.args[i] == "NonDeterministic") {
                        nonDet = true;
                    } else if (m.args[i] == "Deterministic") {
                        continue;
                    } else if (i == 0 && m.args.size() >= 2 && !m.args[i].empty()) {
                        funcModule = m.args[i];
                        hasExplicitMod = true;
                    } else if (!m.args[i].empty()) {
                        cap = m.args[i];
                    }
                }
            }
        }

        std::vector<Tok> prefix;
        int angleDepth = 0;
        int parenDepth = 0;
        bool isOperatorDecl = false;
        while (!atEnd()) {
            const auto k = cur().kind;
            if (angleDepth == 0 && parenDepth == 0 && k == TokKind::Ident && cur().text == "operator") {
                isOperatorDecl = true;
                consumeOperatorNameTokens(prefix);
                break;
            }
            if (angleDepth > 0 && k == TokKind::LParen) { ++parenDepth; prefix.push_back(advance()); continue; }
            if (angleDepth > 0 && k == TokKind::RParen && parenDepth > 0) { --parenDepth; prefix.push_back(advance()); continue; }
            if (parenDepth == 0 && k == TokKind::LAngle) { ++angleDepth; prefix.push_back(advance()); continue; }
            if (parenDepth == 0 && k == TokKind::RAngle && angleDepth > 0) { --angleDepth; prefix.push_back(advance()); continue; }
            if (angleDepth == 0 && parenDepth == 0 &&
                (k == TokKind::LParen || k == TokKind::Semi ||
                 k == TokKind::Equal || k == TokKind::LBrace ||
                 k == TokKind::RBrace)) {
                break;
            }
            prefix.push_back(advance());
        }

        if (cur().kind != TokKind::LParen || !exported || isOperatorDecl) {
            skipToSemicolonOrEndOfBlock();
            return;
        }

        ParsedCallable fn;
        fn.exported = true;
        fn.nonDeterministic = nonDet;
        fn.hasExplicitModule = hasExplicitMod;
        fn.capability = cap;
        fn.moduleOverride = funcModule;
        fn.nsPrefix = currentNsPrefix();
        fn.params = parseParamList(knownTypes_);

        std::vector<Tok> trailingRet;
        bool sawArrow = false;
        while (!atEnd() && cur().kind != TokKind::Semi && cur().kind != TokKind::LBrace && cur().kind != TokKind::RBrace) {
            if (cur().kind == TokKind::Arrow) {
                sawArrow = true;
                advance();
                continue;
            }
            if (sawArrow) {
                if (checkIdent("noexcept")) {
                    sawArrow = false;
                } else {
                    trailingRet.push_back(advance());
                    continue;
                }
            }
            if (checkIdent("noexcept")) {
                advance();
                if (cur().kind == TokKind::LParen) skipBalanced(TokKind::LParen, TokKind::RParen);
            } else {
                advance();
            }
        }
        skipToSemicolonOrEndOfBlock();

        if (prefix.size() < 2 || prefix.back().kind != TokKind::Ident) return;
        fn.name = prefix.back().text;
        fn.qualifiedName = fn.nsPrefix + fn.name;
        prefix.pop_back();
        std::vector<Tok> retToks;
        for (const auto& pt : prefix) {
            if (pt.kind == TokKind::Ident && isQualifierWord(pt.text)) continue;
            retToks.push_back(pt);
        }
        fn.returnCppType = !trailingRet.empty()
            ? formatTokensNormalized(trailingRet)
            : formatTokensNormalized(retToks);
        freeFunctions.push_back(std::move(fn));
    }

    void parseTopLevelItem() {
        auto macros = consumeExportMacros();
        if (atEnd()) return;

        if (cur().kind == TokKind::Semi) {
            advance();
            return;
        }
        if (checkIdent("namespace") || (checkIdent("inline") && peek().kind == TokKind::Ident && peek().text == "namespace")) {
            parseNamespace();
            return;
        }
        if (checkIdent("extern") && peek().kind == TokKind::StringLit) {
            advance(); // extern
            advance(); // "C"
            if (cur().kind == TokKind::LBrace) {
                advance();
                while (!atEnd() && cur().kind != TokKind::RBrace) parseTopLevelItem();
                if (cur().kind == TokKind::RBrace) advance();
            } else {
                parseTopLevelItem();
            }
            return;
        }
        if (checkIdent("using") || checkIdent("typedef")) {
            parseUsingOrTypedef();
            return;
        }
        if (checkIdent("template")) {
            advance();
            if (cur().kind == TokKind::LAngle) skipBalanced(TokKind::LAngle, TokKind::RAngle);
            skipToSemicolonOrEndOfBlock();
            return;
        }
        if (checkIdent("enum")) {
            bool enumExp = false;
            std::string enumMod;
            for (const auto& m : macros) {
                if (m.name == "RS_EXPORT_ENUM") {
                    enumExp = true;
                    if (!m.args.empty() && !m.args[0].empty()) enumMod = m.args[0];
                }
            }
            parseEnum(enumExp, std::move(enumMod));
            return;
        }
        if (checkIdent("class") || checkIdent("struct")) {
            bool expClass = false;
            bool expStruct = false;
            std::string explicitModName;
            for (const auto& m : macros) {
                if (m.name == "RS_EXPORT_CLASS") {
                    expClass = true;
                    expStruct = false;
                    if (!m.args.empty() && !m.args[0].empty()) explicitModName = m.args[0];
                } else if (m.name == "RS_EXPORT_STRUCT" || m.name == "RS_EXPORT_ECS_COMPONENT") {
                    expStruct = true;
                    expClass = false;
                    if (!m.args.empty() && !m.args[0].empty()) explicitModName = m.args[0];
                }
            }
            std::size_t scan = pos_ + 1;
            bool sawLParenBeforeBrace = false;
            while (scan < tok_.size() &&
                   tok_[scan].kind != TokKind::LBrace &&
                   tok_[scan].kind != TokKind::Semi) {
                if (tok_[scan].kind == TokKind::LParen) {
                    sawLParenBeforeBrace = true;
                    break;
                }
                ++scan;
            }
            if (!sawLParenBeforeBrace) {
                parseRecord(expClass, expStruct, std::move(explicitModName));
                return;
            }
        }
        parseTopLevelFunction(macros);
    }

    std::unordered_set<std::string> knownTypes_;
};

inline std::string sanitizeQualifiedCppIdent(std::string_view qName) {
    std::string out;
    out.reserve(qName.size());
    for (std::size_t i = 0; i < qName.size(); ++i) {
        const char ch = qName[i];
        if (ch == ':' && i + 1 < qName.size() && qName[i + 1] == ':') {
            out.push_back('_');
            ++i;
        } else if (isIdentCont(ch)) {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

// ============================================================================
// 3. Binding & .d.rscript Stub Code Generator
// ============================================================================
inline GameApi::HeaderBindingGenerationResult generateFromParsedHeader(
    std::string_view headerContent,
    const std::string& defaultModuleName,
    const std::unordered_map<std::string, std::string>& predefinedMacros) {
    GameApi::HeaderBindingGenerationResult res;

    const std::string preprocessed = preprocessCppHeader(headerContent, predefinedMacros);
    const auto tokens = tokenizeCppDeclarations(preprocessed);
    CppDeclarationParser parser(tokens, defaultModuleName);
    parser.parseTranslationUnit();

    const auto stripLeadingQuals = [](std::string s) -> std::string {
        s = trimView(s);
        bool stripped = true;
        while (stripped) {
            stripped = false;
            for (const char* kw : {
                     "virtual ", "inline ", "constexpr ", "static ",
                     "explicit ", "friend ", "extern ", "const ", "volatile ",
                     "struct ", "class ", "enum ", "typename "}) {
                const std::size_t kwLen = std::strlen(kw);
                if (s.rfind(kw, 0) == 0) {
                    s = trimView(std::string_view(s).substr(kwLen));
                    stripped = true;
                }
            }
        }
        return s;
    };

    const auto extractFirstTemplateArg = [](std::string_view inner) -> std::string {
        int depth = 0;
        for (std::size_t i = 0; i < inner.size(); ++i) {
            const char c = inner[i];
            if (c == '<' || c == '(' || c == '[') ++depth;
            else if (c == '>' || c == ')' || c == ']') --depth;
            else if (c == ',' && depth == 0) {
                return trimView(inner.substr(0, i));
            }
        }
        return trimView(inner);
    };

    // Qualify unqualified namespace-local types for global-scope C++ registration code
    const auto qualifyCppTypeForGlobalScope = [&](
        const std::string& rawCppType,
        const std::string& nsPrefix,
        const std::string& classScopePrefix = {}) -> std::string {
        if (rawCppType.empty() || (nsPrefix.empty() && classScopePrefix.empty())) {
            return rawCppType;
        }
        static const std::unordered_set<std::string> cppKeywordsAndBuiltins = {
            "const", "volatile", "unsigned", "signed", "int", "long", "short",
            "char", "float", "double", "bool", "void", "wchar_t", "char8_t",
            "char16_t", "char32_t", "int8_t", "uint8_t", "int16_t", "uint16_t",
            "int32_t", "uint32_t", "int64_t", "uint64_t", "size_t", "ssize_t",
            "ptrdiff_t", "intptr_t", "uintptr_t", "struct", "class", "enum",
            "typename", "std", "realscript",
        };

        std::vector<std::string> candidatePrefixes;
        if (!classScopePrefix.empty()) {
            candidatePrefixes.push_back(classScopePrefix);
        }
        std::string curNs = nsPrefix;
        while (!curNs.empty()) {
            candidatePrefixes.push_back(curNs);
            if (curNs.size() >= 2 && curNs.substr(curNs.size() - 2) == "::") {
                curNs.erase(curNs.size() - 2);
            }
            const auto pos = curNs.rfind("::");
            if (pos == std::string::npos) {
                curNs.clear();
            } else {
                curNs = curNs.substr(0, pos + 2);
            }
        }

        auto tToks = tokenizeCppDeclarations(rawCppType);
        if (!tToks.empty() && tToks.back().kind == TokKind::Eof) {
            tToks.pop_back();
        }

        std::vector<Tok> qualifiedToks;
        qualifiedToks.reserve(tToks.size() + 4);
        for (std::size_t i = 0; i < tToks.size(); ++i) {
            const auto& tk = tToks[i];
            const bool precededByColonColon = (i > 0 && tToks[i - 1].kind == TokKind::ColonColon);
            const bool followedByColonColon = (i + 1 < tToks.size() && tToks[i + 1].kind == TokKind::ColonColon);

            if (tk.kind == TokKind::Ident &&
                !precededByColonColon &&
                cppKeywordsAndBuiltins.find(tk.text) == cppKeywordsAndBuiltins.end()) {
                std::string matchedPrefix;
                if (!followedByColonColon) {
                    for (const auto& pref : candidatePrefixes) {
                        if (parser.declaredQualifiedTypes.find(pref + tk.text) !=
                            parser.declaredQualifiedTypes.end()) {
                            matchedPrefix = pref;
                            break;
                        }
                    }
                } else {
                    for (const auto& pref : candidatePrefixes) {
                        const std::string subPref = pref + tk.text + "::";
                        bool anyMatch = false;
                        for (const auto& declQ : parser.declaredQualifiedTypes) {
                            if (declQ.rfind(subPref, 0) == 0) {
                                anyMatch = true;
                                break;
                            }
                        }
                        if (anyMatch) {
                            matchedPrefix = pref;
                            break;
                        }
                    }
                }
                if (!matchedPrefix.empty()) {
                    auto pToks = tokenizeCppDeclarations(matchedPrefix);
                    if (!pToks.empty() && pToks.back().kind == TokKind::Eof) pToks.pop_back();
                    for (auto& pt : pToks) qualifiedToks.push_back(std::move(pt));
                }
            }
            qualifiedToks.push_back(tk);
        }
        return CppDeclarationParser::formatTokensNormalized(qualifiedToks);
    };

    const auto tryMapRsTypeToNativeFieldKind = [](const std::string& rsType) -> std::string {
        if (rsType == "sbyte") return "realscript::runtime::NativeFieldKind::Int8";
        if (rsType == "byte") return "realscript::runtime::NativeFieldKind::UInt8";
        if (rsType == "short") return "realscript::runtime::NativeFieldKind::Int16";
        if (rsType == "ushort") return "realscript::runtime::NativeFieldKind::UInt16";
        if (rsType == "int") return "realscript::runtime::NativeFieldKind::Int32";
        if (rsType == "uint") return "realscript::runtime::NativeFieldKind::UInt32";
        if (rsType == "long") return "realscript::runtime::NativeFieldKind::Int64";
        if (rsType == "ulong") return "realscript::runtime::NativeFieldKind::UInt64";
        if (rsType == "float") return "realscript::runtime::NativeFieldKind::Float32";
        if (rsType == "double") return "realscript::runtime::NativeFieldKind::Float64";
        if (rsType == "bool") return "realscript::runtime::NativeFieldKind::Bool";
        if (rsType == "Fixed") return "realscript::runtime::NativeFieldKind::Fixed";
        if (rsType == "Fixed16") return "realscript::runtime::NativeFieldKind::Fixed16";
        if (rsType == "Fixed64") return "realscript::runtime::NativeFieldKind::Fixed64";
        if (rsType == "Vec2") return "realscript::runtime::NativeFieldKind::Vec2";
        if (rsType == "Vec3") return "realscript::runtime::NativeFieldKind::Vec3";
        if (rsType == "Quat") return "realscript::runtime::NativeFieldKind::Quat";
        if (rsType == "Vec2f") return "realscript::runtime::NativeFieldKind::Vec2f";
        if (rsType == "Vec3f") return "realscript::runtime::NativeFieldKind::Vec3f";
        if (rsType == "Quatf") return "realscript::runtime::NativeFieldKind::Quatf";
        return {};
    };

    const auto isInvalidRawEcsFieldCppType = [&](std::string cppType, int depth, auto& self) -> bool {
        cppType = stripLeadingQuals(std::move(cppType));
        if (cppType.empty()) return true;
        if (cppType.find('*') != std::string::npos ||
            cppType.find('&') != std::string::npos ||
            cppType.find('[') != std::string::npos ||
            cppType.find("vector<") != std::string::npos ||
            cppType.find("shared_ptr<") != std::string::npos ||
            cppType.find("string") != std::string::npos) {
            return true;
        }
        if (depth < 8) {
            auto aliasIt = parser.typeAliases.find(cppType);
            if (aliasIt == parser.typeAliases.end()) {
                const auto nsPos = cppType.rfind("::");
                if (nsPos != std::string::npos) {
                    aliasIt = parser.typeAliases.find(cppType.substr(nsPos + 2));
                }
            }
            if (aliasIt != parser.typeAliases.end() && aliasIt->second != cppType) {
                return self(aliasIt->second, depth + 1, self);
            }
        }
        return false;
    };

    bool usesMathTypes = false;
    std::function<std::string(std::string, int)> mapCppTypeToRsImpl;
    mapCppTypeToRsImpl = [&](std::string cppType, int aliasDepth) -> std::string {
        cppType = stripLeadingQuals(std::move(cppType));
        while (!cppType.empty() &&
               (cppType.back() == '&' || cppType.back() == '*' || cppType.back() == ' ')) {
            cppType.pop_back();
        }
        while (cppType.size() > 6 && cppType.compare(cppType.size() - 6, 6, " const") == 0) {
            cppType = trimView(std::string_view(cppType).substr(0, cppType.size() - 6));
            while (!cppType.empty() &&
                   (cppType.back() == '&' || cppType.back() == '*' || cppType.back() == ' ')) {
                cppType.pop_back();
            }
        }

        if (aliasDepth < 8) {
            auto aliasIt = parser.typeAliases.find(cppType);
            if (aliasIt == parser.typeAliases.end()) {
                const auto nsPos = cppType.rfind("::");
                if (nsPos != std::string::npos) {
                    aliasIt = parser.typeAliases.find(cppType.substr(nsPos + 2));
                }
            }
            if (aliasIt != parser.typeAliases.end() && aliasIt->second != cppType) {
                return mapCppTypeToRsImpl(aliasIt->second, aliasDepth + 1);
            }
        }

        if (cppType.rfind("std::vector<", 0) == 0 && cppType.back() == '>') {
            const std::string inner = extractFirstTemplateArg(
                std::string_view(cppType).substr(12, cppType.size() - 13));
            return mapCppTypeToRsImpl(inner, aliasDepth + 1) + "[]";
        }
        if (cppType.rfind("vector<", 0) == 0 && cppType.back() == '>') {
            const std::string inner = extractFirstTemplateArg(
                std::string_view(cppType).substr(7, cppType.size() - 8));
            return mapCppTypeToRsImpl(inner, aliasDepth + 1) + "[]";
        }
        if (cppType.rfind("std::shared_ptr<", 0) == 0 && cppType.back() == '>') {
            cppType = trimView(std::string_view(cppType).substr(16, cppType.size() - 17));
        } else if (cppType.rfind("shared_ptr<", 0) == 0 && cppType.back() == '>') {
            cppType = trimView(std::string_view(cppType).substr(11, cppType.size() - 12));
        }
        while (!cppType.empty() &&
               (cppType.back() == '&' || cppType.back() == '*' || cppType.back() == ' ')) {
            cppType.pop_back();
        }

        if (cppType == "int" || cppType == "signed int" || cppType == "signed" ||
            cppType == "int32_t" || cppType == "std::int32_t") return "int";
        if (cppType == "int64_t" || cppType == "std::int64_t" || cppType == "long" ||
            cppType == "long int" || cppType == "long long" || cppType == "long long int" ||
            cppType == "ssize_t" || cppType == "std::ptrdiff_t" || cppType == "ptrdiff_t") return "long";
        if (cppType == "uint32_t" || cppType == "std::uint32_t" ||
            cppType == "unsigned int" || cppType == "unsigned") return "uint";
        if (cppType == "uint64_t" || cppType == "std::uint64_t" ||
            cppType == "unsigned long" || cppType == "unsigned long int" ||
            cppType == "unsigned long long" || cppType == "unsigned long long int" ||
            cppType == "size_t" || cppType == "std::size_t") return "ulong";
        if (cppType == "uint8_t" || cppType == "std::uint8_t" || cppType == "unsigned char") return "byte";
        if (cppType == "int8_t" || cppType == "std::int8_t" || cppType == "signed char") return "sbyte";
        if (cppType == "int16_t" || cppType == "std::int16_t" || cppType == "short" || cppType == "short int") return "short";
        if (cppType == "uint16_t" || cppType == "std::uint16_t" || cppType == "unsigned short" || cppType == "unsigned short int") return "ushort";
        if (cppType == "float") return "float";
        if (cppType == "double") return "double";
        if (cppType == "bool") return "bool";
        if (cppType == "char") return "char";
        if (cppType == "void") return "void";
        if (cppType.find("string") != std::string::npos) return "string";
        if (cppType.find("Vec2f") != std::string::npos) { usesMathTypes = true; return "Vec2f"; }
        if (cppType.find("Vec3f") != std::string::npos) { usesMathTypes = true; return "Vec3f"; }
        if (cppType.find("Quatf") != std::string::npos) { usesMathTypes = true; return "Quatf"; }
        if (cppType.find("Fixed64") != std::string::npos) { usesMathTypes = true; return "Fixed64"; }
        if (cppType.find("Vec2") != std::string::npos) { usesMathTypes = true; return "Vec2"; }
        if (cppType.find("Vec3") != std::string::npos) { usesMathTypes = true; return "Vec3"; }
        if (cppType.find("Quat") != std::string::npos) { usesMathTypes = true; return "Quat"; }
        if (cppType.find("Fixed16") != std::string::npos) { usesMathTypes = true; return "Fixed16"; }
        if (cppType.find("Fixed") != std::string::npos) { usesMathTypes = true; return "Fixed"; }
        const auto nsPos = cppType.rfind("::");
        if (nsPos != std::string::npos) {
            return cppType.substr(nsPos + 2);
        }
        return cppType;
    };

    const auto mapCppTypeToRs = [&](std::string cppType) -> std::string {
        return mapCppTypeToRsImpl(std::move(cppType), 0);
    };

    // Pre-validate ECS structs so invalid/unmapped ECS structs are rejected cleanly
    std::vector<bool> validRecord(parser.records.size(), true);
    for (std::size_t ri = 0; ri < parser.records.size(); ++ri) {
        const auto& rec = parser.records[ri];
        if (!rec.isEcsStruct) continue;
        for (const auto& f : rec.fields) {
            if (f.hasArrayDeclarator ||
                isInvalidRawEcsFieldCppType(f.cppType, 0, isInvalidRawEcsFieldCppType)) {
                validRecord[ri] = false;
                break;
            }
            bool savedMath = usesMathTypes;
            const std::string rsT = mapCppTypeToRs(f.cppType);
            usesMathTypes = savedMath;
            if (tryMapRsTypeToNativeFieldKind(rsT).empty()) {
                validRecord[ri] = false;
                break;
            }
        }
    }

    // Determine effective .d.rscript module name honoring explicit annotation overrides
    std::vector<std::string> explicitModules;
    const auto noteExplicitModule = [&](bool hasExp, const std::string& mod) {
        if (!hasExp || mod.empty()) return;
        if (std::find(explicitModules.begin(), explicitModules.end(), mod) == explicitModules.end()) {
            explicitModules.push_back(mod);
        }
    };
    for (std::size_t ri = 0; ri < parser.records.size(); ++ri) {
        if (!validRecord[ri]) continue;
        noteExplicitModule(parser.records[ri].hasExplicitModule, parser.records[ri].moduleName);
    }
    for (const auto& fn : parser.freeFunctions) {
        if (!fn.exported) continue;
        noteExplicitModule(fn.hasExplicitModule, fn.moduleOverride);
    }
    for (const auto& en : parser.enums) {
        noteExplicitModule(en.hasExplicitModule, en.moduleName);
    }

    std::string stubModuleName = defaultModuleName;
    if (!explicitModules.empty() &&
        std::find(explicitModules.begin(), explicitModules.end(), defaultModuleName) ==
            explicitModules.end()) {
        stubModuleName = explicitModules.front();
    }

    // Count short record names across valid exported records to disambiguate builder_<ns_Cls> collisions
    std::unordered_map<std::string, std::size_t> recordShortNameCounts;
    std::unordered_set<std::string> exportedStructs;
    std::unordered_set<std::string> exportedClasses;
    std::unordered_map<std::string, std::size_t> exportedClassByQualName;
    std::unordered_map<std::string, std::size_t> exportedClassByShortName;
    std::unordered_map<std::string, std::string> exportedEnumFirstMember;
    std::vector<std::string> extraStubImports;

    for (const auto& e : parser.enums) {
        const std::string eMod = e.hasExplicitModule ? e.moduleName : stubModuleName;
        if (eMod == stubModuleName && !e.members.empty()) {
            exportedEnumFirstMember[e.name] = e.members.front().name;
        }
    }
    for (std::size_t ri = 0; ri < parser.records.size(); ++ri) {
        if (!validRecord[ri]) continue;
        const auto& r = parser.records[ri];
        ++recordShortNameCounts[r.name];
        if (!r.isEcsStruct) {
            exportedClassByQualName[r.qualifiedName] = ri;
            exportedClassByShortName[r.name] = ri;
        }
        if (r.moduleName == stubModuleName) {
            if (r.isEcsStruct) exportedStructs.insert(r.name);
            else exportedClasses.insert(r.name);
        }
    }

    const auto findExportedBaseRecord = [&](const ParsedRecord& rec) -> const ParsedRecord* {
        for (const auto& b : rec.baseClasses) {
            const std::string qualBase = qualifyCppTypeForGlobalScope(b, rec.nsPrefix);
            if (const auto qIt = exportedClassByQualName.find(qualBase);
                qIt != exportedClassByQualName.end() &&
                parser.records[qIt->second].qualifiedName != rec.qualifiedName) {
                return &parser.records[qIt->second];
            }
            const auto nsPos = b.rfind("::");
            const std::string shortBase = nsPos == std::string::npos ? b : b.substr(nsPos + 2);
            if (recordShortNameCounts[shortBase] == 1) {
                if (const auto sIt = exportedClassByShortName.find(shortBase);
                    sIt != exportedClassByShortName.end() &&
                    parser.records[sIt->second].qualifiedName != rec.qualifiedName) {
                    return &parser.records[sIt->second];
                }
            }
        }
        return nullptr;
    };

    const auto dummyRsReturn = [&](const std::string& rsRet) -> std::string {
        if (rsRet == "void") return "return;";
        if (rsRet == "bool") return "return false;";
        if (rsRet == "string") return "return \"\";";
        if (rsRet == "float") return "return (float)0.0;";
        if (rsRet == "double") return "return 0.0;";
        if (rsRet == "char") return "return 'a';";
        if (rsRet == "int" || rsRet == "long") return "return 0;";
        if (rsRet == "uint" || rsRet == "ulong" || rsRet == "byte" ||
            rsRet == "sbyte" || rsRet == "short" || rsRet == "ushort") {
            return "return (" + rsRet + ")0;";
        }
        if (rsRet == "Vec2" || rsRet == "Vec3" || rsRet == "Quat" ||
            rsRet == "Vec2f" || rsRet == "Vec3f" || rsRet == "Quatf" ||
            rsRet == "Fixed" || rsRet == "Fixed16" || rsRet == "Fixed64" ||
            exportedStructs.count(rsRet) > 0) {
            return "return new " + rsRet + "();";
        }
        if (const auto it = exportedEnumFirstMember.find(rsRet);
            it != exportedEnumFirstMember.end() && !it->second.empty()) {
            return "return " + rsRet + "." + it->second + ";";
        }
        return "return null;";
    };

    const auto buildDetArg = [](bool nonDet, const std::string& cap) -> std::string {
        if (!nonDet && cap.empty()) return {};
        std::string out = nonDet
            ? ", realscript::runtime::BindingDeterminism::NonDeterministic"
            : ", realscript::runtime::BindingDeterminism::Deterministic";
        if (!cap.empty()) out += ", \"" + cap + "\"";
        return out;
    };

    std::ostringstream cppBodyOut;
    std::ostringstream cppHelpersOut;
    std::ostringstream rsBodyOut;

    // 1. Emit Enums
    for (const auto& en : parser.enums) {
        const std::string eMod = en.hasExplicitModule ? en.moduleName : stubModuleName;
        ++res.enumsExported;
        if (eMod == stubModuleName) {
            rsBodyOut << "enum " << en.name << " {\n";
            for (const auto& mem : en.members) {
                rsBodyOut << "    " << mem.name << " = " << mem.exprText << ",\n";
            }
            rsBodyOut << "}\n\n";
        }
    }

    // 2. Emit Classes & Validated ECS Structs
    for (std::size_t ri = 0; ri < parser.records.size(); ++ri) {
        const auto& rec = parser.records[ri];
        if (!validRecord[ri]) {
            cppBodyOut << "    // Skipped ECS struct " << rec.qualifiedName
                       << ": contains unsupported/unmapped ECS field type\n";
            continue;
        }

        const std::string varSuffix = (recordShortNameCounts[rec.name] > 1)
            ? sanitizeQualifiedCppIdent(rec.qualifiedName)
            : rec.name;
        const bool emitInStub = (rec.moduleName == stubModuleName);
        const std::string classScopePrefix = rec.qualifiedName + "::";

        if (rec.isEcsStruct) {
            ++res.structsExported;
            if (emitInStub) {
                rsBodyOut << "struct " << rec.name << " {\n";
            }
            cppBodyOut << "    auto struct_" << varSuffix << " = api.structType<" << rec.qualifiedName
                       << ">(\"" << rec.moduleName << "\", \"" << rec.name << "\");\n";
            for (const auto& f : rec.fields) {
                const std::string rsType = mapCppTypeToRs(f.cppType);
                if (emitInStub) {
                    rsBodyOut << "    public " << rsType << " " << f.name << ";\n";
                }
                cppBodyOut << "    struct_" << varSuffix << ".field<&" << rec.qualifiedName
                           << "::" << f.name << ">(\"" << f.name << "\");\n";
            }
            if (emitInStub) {
                rsBodyOut << "}\n\n";
            }

            cppHelpersOut << "inline bool bindEcsColumn_" << varSuffix << "(\n"
                          << "    realscript::game::ScriptRuntime& runtime,\n"
                          << "    " << rec.qualifiedName << "* data,\n"
                          << "    std::size_t count,\n"
                          << "    realscript::runtime::RuntimeError& error,\n"
                          << "    bool readOnly = false) {\n"
                          << "    static_assert(std::is_standard_layout_v<" << rec.qualifiedName << ">,\n"
                          << "        \"ECS component struct must satisfy std::is_standard_layout_v\");\n"
                          << "    const std::vector<realscript::runtime::NativeFieldLayout> fields = {\n";
            for (const auto& f : rec.fields) {
                const std::string rsType = mapCppTypeToRs(f.cppType);
                cppHelpersOut << "        {offsetof(" << rec.qualifiedName << ", " << f.name << "), "
                              << tryMapRsTypeToNativeFieldKind(rsType) << "},\n";
            }
            cppHelpersOut << "    };\n"
                          << "    return runtime.bindEcsRawColumn(\n"
                          << "        \"" << rec.moduleName << "::" << rec.name << "\",\n"
                          << "        data,\n"
                          << "        count,\n"
                          << "        sizeof(" << rec.qualifiedName << "),\n"
                          << "        fields,\n"
                          << "        error,\n"
                          << "        readOnly);\n"
                          << "}\n\n";
            continue;
        }

        ++res.classesExported;
        const std::string builderVar = "builder_" + varSuffix;
        cppBodyOut << "    auto " << builderVar << " = api.type<" << rec.qualifiedName
                   << ">(\"" << rec.moduleName << "\", \"" << rec.name << "\");\n";

        const ParsedRecord* exportedBaseRec = findExportedBaseRecord(rec);
        if (exportedBaseRec != nullptr) {
            cppBodyOut << "    " << builderVar << ".base<" << exportedBaseRec->qualifiedName << ">();\n";
        }

        if (emitInStub) {
            if (exportedBaseRec != nullptr) {
                if (exportedBaseRec->moduleName != stubModuleName &&
                    std::find(extraStubImports.begin(), extraStubImports.end(), exportedBaseRec->moduleName) ==
                        extraStubImports.end()) {
                    extraStubImports.push_back(exportedBaseRec->moduleName);
                }
                rsBodyOut << "class " << rec.name << " : " << exportedBaseRec->name << " {\n";
            } else {
                rsBodyOut << "class " << rec.name << " {\n    handle __native;\n";
            }
        }

        std::unordered_map<std::string, std::size_t> methodNameCounts;
        for (const auto& m : rec.methods) {
            if (m.exported && !m.isConstructor) {
                ++methodNameCounts[m.name];
            }
        }

        for (const auto& m : rec.methods) {
            if (!m.exported) continue;
            const std::string detArg = buildDetArg(m.nonDeterministic, m.capability);
            if (emitInStub) {
                if (m.nonDeterministic) rsBodyOut << "    [NonDeterministic]\n";
                if (!m.capability.empty()) rsBodyOut << "    [Capability(\"" << m.capability << "\")]\n";
            }

            if (m.isConstructor) {
                ++res.constructorsExported;
                ++res.methodsExported;
                cppBodyOut << "    " << builderVar << ".constructor<";
                for (std::size_t i = 0; i < m.params.size(); ++i) {
                    if (i != 0) cppBodyOut << ", ";
                    cppBodyOut << qualifyCppTypeForGlobalScope(
                        m.params[i].rawCppType, rec.nsPrefix, classScopePrefix);
                }
                cppBodyOut << ">(\"" << m.name << "\"" << detArg << ");\n";
                if (emitInStub) {
                    rsBodyOut << "    public static " << rec.name << " " << m.name << "(";
                    for (std::size_t i = 0; i < m.params.size(); ++i) {
                        if (i != 0) rsBodyOut << ", ";
                        rsBodyOut << mapCppTypeToRs(m.params[i].rawCppType) << " " << m.params[i].name;
                    }
                    rsBodyOut << ") { " << dummyRsReturn(rec.name) << " }\n";
                }
            } else {
                ++res.methodsExported;
                const std::string rsRet = mapCppTypeToRs(m.returnCppType);
                if (methodNameCounts[m.name] > 1) {
                    const std::string qualRet = qualifyCppTypeForGlobalScope(
                        m.returnCppType, rec.nsPrefix, classScopePrefix);
                    std::ostringstream sig;
                    if (m.isStatic) {
                        sig << "static_cast<" << qualRet << " (*)(";
                    } else {
                        sig << "static_cast<" << qualRet << " (" << rec.qualifiedName << "::*)(";
                    }
                    for (std::size_t i = 0; i < m.params.size(); ++i) {
                        if (i != 0) sig << ", ";
                        sig << qualifyCppTypeForGlobalScope(
                            m.params[i].rawCppType, rec.nsPrefix, classScopePrefix);
                    }
                    sig << ")" << (m.isConst && !m.isStatic ? " const" : "")
                        << ">(&" << rec.qualifiedName << "::" << m.name << ")";
                    cppBodyOut << "    " << builderVar << ".method<" << sig.str()
                               << ">(\"" << m.name << "\"" << detArg << ");\n";
                } else {
                    cppBodyOut << "    " << builderVar << ".method<&" << rec.qualifiedName
                               << "::" << m.name << ">(\"" << m.name << "\"" << detArg << ");\n";
                }
                if (emitInStub) {
                    rsBodyOut << "    " << (m.isStatic ? "public static " : "") << rsRet << " " << m.name << "(";
                    for (std::size_t i = 0; i < m.params.size(); ++i) {
                        if (i != 0) rsBodyOut << ", ";
                        rsBodyOut << mapCppTypeToRs(m.params[i].rawCppType) << " " << m.params[i].name;
                    }
                    rsBodyOut << ") { " << dummyRsReturn(rsRet) << " }\n";
                }
            }
        }

        for (const auto& fld : rec.fields) {
            if (!fld.exported) continue;
            ++res.propertiesExported;
            const std::string rsType = mapCppTypeToRs(fld.cppType);
            cppBodyOut << "    " << builderVar << ".field<&" << rec.qualifiedName
                       << "::" << fld.name << ">(\"" << fld.name << "\");\n";
            if (emitInStub) {
                rsBodyOut << "    " << rsType << " " << fld.name << " {\n"
                          << "        get { " << dummyRsReturn(rsType) << " }\n";
                if (!fld.readOnly) {
                    rsBodyOut << "        set { return; }\n";
                }
                rsBodyOut << "    }\n";
            }
        }

        for (const auto& prop : rec.properties) {
            ++res.propertiesExported;
            std::string propRsType = "int";
            if (!prop.explicitCppType.empty()) {
                propRsType = mapCppTypeToRs(prop.explicitCppType);
            } else {
                const ParsedRecord* curRec = &rec;
                for (int depth = 0; curRec != nullptr && depth < 16; ++depth) {
                    bool found = false;
                    for (const auto& m : curRec->methods) {
                        if (!m.isConstructor && m.name == prop.getterMethod && !m.returnCppType.empty()) {
                            propRsType = mapCppTypeToRs(m.returnCppType);
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                    curRec = findExportedBaseRecord(*curRec);
                }
            }
            if (!prop.setterMethod.empty()) {
                cppBodyOut << "    " << builderVar << ".property<&" << rec.qualifiedName
                           << "::" << prop.getterMethod << ", &" << rec.qualifiedName
                           << "::" << prop.setterMethod << ">(\"" << prop.propName << "\");\n";
                if (emitInStub) {
                    rsBodyOut << "    " << propRsType << " " << prop.propName << " {\n"
                              << "        get { " << dummyRsReturn(propRsType) << " }\n"
                              << "        set { return; }\n"
                              << "    }\n";
                }
            } else {
                cppBodyOut << "    " << builderVar << ".property<&" << rec.qualifiedName
                           << "::" << prop.getterMethod << ">(\"" << prop.propName << "\");\n";
                if (emitInStub) {
                    rsBodyOut << "    " << propRsType << " " << prop.propName << " {\n"
                              << "        get { " << dummyRsReturn(propRsType) << " }\n"
                              << "    }\n";
                }
            }
        }

        if (emitInStub) {
            rsBodyOut << "}\n\n";
        }
    }

    // 3. Emit Free Functions (with automatic namespace-qualified overload disambiguation)
    std::unordered_map<std::string, std::size_t> freeFuncCounts;
    for (const auto& fn : parser.freeFunctions) {
        if (fn.exported) ++freeFuncCounts[fn.qualifiedName];
    }
    for (const auto& fn : parser.freeFunctions) {
        if (!fn.exported) continue;
        ++res.functionsExported;
        const std::string detArg = buildDetArg(fn.nonDeterministic, fn.capability);
        const std::string rsRet = mapCppTypeToRs(fn.returnCppType);
        const bool emitInStub = (fn.moduleOverride == stubModuleName);

        if (freeFuncCounts[fn.qualifiedName] > 1) {
            const std::string qualRet = qualifyCppTypeForGlobalScope(fn.returnCppType, fn.nsPrefix);
            std::ostringstream sig;
            sig << "static_cast<" << qualRet << " (*)(";
            for (std::size_t i = 0; i < fn.params.size(); ++i) {
                if (i != 0) sig << ", ";
                sig << qualifyCppTypeForGlobalScope(fn.params[i].rawCppType, fn.nsPrefix);
            }
            sig << ")>(&" << fn.qualifiedName << ")";
            cppBodyOut << "    api.function(\"" << fn.moduleOverride << "\", \""
                       << fn.name << "\", " << sig.str() << detArg << ");\n";
        } else {
            cppBodyOut << "    api.function(\"" << fn.moduleOverride << "\", \""
                       << fn.name << "\", &" << fn.qualifiedName << detArg << ");\n";
        }
        if (emitInStub) {
            if (fn.nonDeterministic) rsBodyOut << "[NonDeterministic]\n";
            if (!fn.capability.empty()) rsBodyOut << "[Capability(\"" << fn.capability << "\")]\n";
            rsBodyOut << rsRet << " " << fn.name << "(";
            for (std::size_t i = 0; i < fn.params.size(); ++i) {
                if (i != 0) rsBodyOut << ", ";
                rsBodyOut << mapCppTypeToRs(fn.params[i].rawCppType) << " " << fn.params[i].name;
            }
            rsBodyOut << ") { " << dummyRsReturn(rsRet) << " }\n";
        }
    }

    std::ostringstream cppOut;
    cppOut << "// Auto-generated RealScript C++ Binding Registration & Zero-Copy ECS Helpers\n";
    cppOut << "#pragma once\n\n";
    cppOut << "#include <cstddef>\n";
    cppOut << "#include <type_traits>\n";
    cppOut << "#include <vector>\n";
    cppOut << "#include \"realscript/game/GameScripting.h\"\n\n";
    if (!cppHelpersOut.str().empty()) {
        cppOut << cppHelpersOut.str();
    }
    cppOut << "inline void registerGeneratedHeaderBindings(realscript::game::GameApi& api) {\n";
    cppOut << "    (void)api;\n";
    cppOut << cppBodyOut.str();
    cppOut << "}\n";

    std::ostringstream rsOut;
    rsOut << "// Auto-generated RealScript Stub from C++ Header\n";
    rsOut << "module " << stubModuleName << ";\n";
    if (usesMathTypes && stubModuleName != "RealScript.Math" &&
        std::find(extraStubImports.begin(), extraStubImports.end(), "RealScript.Math") ==
            extraStubImports.end()) {
        rsOut << "import RealScript.Math;\n";
    }
    for (const auto& imp : extraStubImports) {
        if (!imp.empty() && imp != stubModuleName) {
            rsOut << "import " << imp << ";\n";
        }
    }
    rsOut << "\n" << rsBodyOut.str();

    res.cppRegistrationSource = cppOut.str();
    res.scriptDeclarationStub = rsOut.str();
    return res;
}

} // namespace realscript::game::rsbind_detail
