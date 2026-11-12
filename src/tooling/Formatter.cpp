#include "realscript/tooling/Formatter.h"

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realscript::tooling {
namespace {

struct TriviaComment {
    bool isLineComment = false;
    std::size_t newlinesBefore = 0;
    std::string text;
};

enum class FmtExtraKind : std::uint8_t {
    None = 0,
    PlusPlus,
    MinusMinus,
    ColonColon,
};

enum class BraceRole : std::uint8_t {
    Block = 0,
    EmptyBlock,
    AutoProperty,
    InlineInitializer,
    SwitchStatement,
    SwitchExpression,
    EnumBody,
};

enum class ParenRole : std::uint8_t {
    Normal = 0,
    Control,
    ForHeader,
    Cast,
    LambdaParams,
};

enum class ColonRole : std::uint8_t {
    Spacing = 0,   // inheritance `: Base`, constraint `where T : IFoo`, ternary `? a : b`, ctor `: base()`
    SwitchLabel,   // `case 1:` or `default:`
    NamedArgument, // `foo(bar: 1)`
};

struct FmtToken {
    syntax::SyntaxKind kind = syntax::SyntaxKind::BadToken;
    FmtExtraKind extraKind = FmtExtraKind::None;
    text::TextSpan span{};
    std::string text;

    std::vector<TriviaComment> leadingComments;
    std::size_t newlinesAfterComments = 0;

    bool isGenericOpen = false;
    bool isGenericClose = false;
    bool isAttributeOpen = false;
    bool isAttributeClose = false;
    bool isUnaryPrefix = false;
    bool isUnaryPostfix = false;
    bool isNullableOrCondAccessQuestion = false;

    BraceRole braceRole = BraceRole::Block;
    ParenRole parenRole = ParenRole::Normal;
    ColonRole colonRole = ColonRole::Spacing;
    std::size_t matchingIndex = 0;
};

std::string normalizeLineEndings(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                ++i;
            }
            out.push_back('\n');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

std::string trimTrailingHorizSpace(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return std::string(text.substr(0, end));
}

void parseTriviaSlice(
    std::string_view slice,
    std::vector<TriviaComment>& outComments,
    std::size_t& outTrailingNewlines) {
    outComments.clear();
    std::size_t newlines = 0;
    std::size_t pos = 0;
    while (pos < slice.size()) {
        const char ch = slice[pos];
        if (ch == '\n') {
            ++newlines;
            ++pos;
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            ++pos;
            continue;
        }
        if (ch == '/' && pos + 1 < slice.size() && slice[pos + 1] == '/') {
            const std::size_t start = pos;
            pos += 2;
            while (pos < slice.size() && slice[pos] != '\n') {
                ++pos;
            }
            TriviaComment c;
            c.isLineComment = true;
            c.newlinesBefore = newlines;
            c.text = trimTrailingHorizSpace(slice.substr(start, pos - start));
            outComments.push_back(std::move(c));
            newlines = 0;
            continue;
        }
        if (ch == '/' && pos + 1 < slice.size() && slice[pos + 1] == '*') {
            const std::size_t start = pos;
            pos += 2;
            while (pos + 1 < slice.size() && !(slice[pos] == '*' && slice[pos + 1] == '/')) {
                ++pos;
            }
            if (pos + 1 < slice.size()) {
                pos += 2;
            } else {
                pos = slice.size();
            }
            TriviaComment c;
            c.isLineComment = false;
            c.newlinesBefore = newlines;
            c.text = std::string(slice.substr(start, pos - start));
            outComments.push_back(std::move(c));
            newlines = 0;
            continue;
        }
        ++pos;
    }
    outTrailingNewlines = newlines;
}

bool isWordLikeKind(syntax::SyntaxKind kind) noexcept {
    if (kind == syntax::SyntaxKind::IdentifierToken ||
        kind == syntax::SyntaxKind::IntegerLiteralToken ||
        kind == syntax::SyntaxKind::FloatLiteralToken) {
        return true;
    }
    return kind >= syntax::SyntaxKind::ModuleKeyword &&
        kind <= syntax::SyntaxKind::VoidKeyword;
}

bool isIdentifierOrTypeWord(const FmtToken& tok) noexcept {
    if (tok.extraKind != FmtExtraKind::None) return false;
    return tok.kind == syntax::SyntaxKind::IdentifierToken ||
        syntax::isPrimitiveTypeKeyword(tok.kind) ||
        tok.kind == syntax::SyntaxKind::VoidKeyword;
}

bool isAllowedInsideGeneric(const std::vector<FmtToken>& tokens, std::size_t idx) noexcept {
    const auto& tok = tokens[idx];
    if (tok.extraKind == FmtExtraKind::ColonColon) return true;
    if (tok.extraKind != FmtExtraKind::None) return false;
    if (isIdentifierOrTypeWord(tok)) return true;
    switch (tok.kind) {
    case syntax::SyntaxKind::LessToken:
    case syntax::SyntaxKind::GreaterToken:
    case syntax::SyntaxKind::CommaToken:
    case syntax::SyntaxKind::DotToken:
    case syntax::SyntaxKind::QuestionToken:
        return true;
    case syntax::SyntaxKind::OpenBracketToken:
        return idx + 1 < tokens.size() &&
            tokens[idx + 1].kind == syntax::SyntaxKind::CloseBracketToken &&
            tokens[idx + 1].extraKind == FmtExtraKind::None;
    case syntax::SyntaxKind::CloseBracketToken:
        return idx > 0 &&
            tokens[idx - 1].kind == syntax::SyntaxKind::OpenBracketToken &&
            tokens[idx - 1].extraKind == FmtExtraKind::None;
    default:
        return false;
    }
}

bool startsOperandAfterCast(const FmtToken& tok) noexcept {
    if (tok.extraKind == FmtExtraKind::PlusPlus || tok.extraKind == FmtExtraKind::MinusMinus) {
        return true;
    }
    switch (tok.kind) {
    case syntax::SyntaxKind::IdentifierToken:
    case syntax::SyntaxKind::IntegerLiteralToken:
    case syntax::SyntaxKind::FloatLiteralToken:
    case syntax::SyntaxKind::StringLiteralToken:
    case syntax::SyntaxKind::InterpolatedStringToken:
    case syntax::SyntaxKind::TrueKeyword:
    case syntax::SyntaxKind::FalseKeyword:
    case syntax::SyntaxKind::NullKeyword:
    case syntax::SyntaxKind::ThisKeyword:
    case syntax::SyntaxKind::BaseKeyword:
    case syntax::SyntaxKind::NewKeyword:
    case syntax::SyntaxKind::TypeofKeyword:
    case syntax::SyntaxKind::OpenParenToken:
    case syntax::SyntaxKind::BangToken:
        return true;
    default:
        return false;
    }
}

void classifyGenerics(std::vector<FmtToken>& tokens) {
    const std::size_t n = tokens.size();
    const auto findMatchingGreater = [&](std::size_t openIdx, bool& outHasTopComma) -> std::optional<std::size_t> {
        outHasTopComma = false;
        if (openIdx == 0 || openIdx + 2 >= n) return std::nullopt;
        const auto& prev = tokens[openIdx - 1];
        if (!isIdentifierOrTypeWord(prev) && prev.kind != syntax::SyntaxKind::SequenceKeyword) {
            return std::nullopt;
        }
        int depth = 1;
        std::size_t idx = openIdx + 1;
        if (idx < n && tokens[idx].kind == syntax::SyntaxKind::GreaterToken) {
            return std::nullopt; // `<>` is empty, not valid in RealScript generics
        }
        while (idx < n && depth > 0) {
            if (tokens[idx].kind == syntax::SyntaxKind::EndOfFileToken) return std::nullopt;
            if (!isAllowedInsideGeneric(tokens, idx)) return std::nullopt;
            if (tokens[idx].kind == syntax::SyntaxKind::LessToken) {
                const auto& beforeInner = tokens[idx - 1];
                if (!isIdentifierOrTypeWord(beforeInner)) return std::nullopt;
                ++depth;
            } else if (tokens[idx].kind == syntax::SyntaxKind::GreaterToken) {
                --depth;
                if (depth == 0) return idx;
            } else if (tokens[idx].kind == syntax::SyntaxKind::CommaToken && depth == 1) {
                outHasTopComma = true;
            }
            ++idx;
        }
        return std::nullopt;
    };

    const auto markNestedGenerics = [&](auto& self, std::size_t openIdx, std::size_t closeIdx) -> void {
        tokens[openIdx].isGenericOpen = true;
        tokens[closeIdx].isGenericClose = true;
        tokens[openIdx].matchingIndex = closeIdx;
        tokens[closeIdx].matchingIndex = openIdx;

        for (std::size_t i = openIdx + 1; i < closeIdx; ++i) {
            if (tokens[i].kind == syntax::SyntaxKind::LessToken && !tokens[i].isGenericOpen) {
                bool dummyComma = false;
                if (const auto innerClose = findMatchingGreater(i, dummyComma);
                    innerClose && *innerClose < closeIdx) {
                    self(self, i, *innerClose);
                    i = *innerClose;
                }
            }
        }
    };

    for (std::size_t i = 0; i < n; ++i) {
        if (tokens[i].kind != syntax::SyntaxKind::LessToken || tokens[i].isGenericOpen) {
            continue;
        }
        bool hasTopComma = false;
        const auto closeOpt = findMatchingGreater(i, hasTopComma);
        if (!closeOpt) continue;
        const std::size_t closeIdx = *closeOpt;

        // Walk backward across qualified type name `Mod.Sub::Type`
        std::size_t baseIdx = i - 1;
        while (baseIdx >= 2 &&
               (tokens[baseIdx - 1].kind == syntax::SyntaxKind::DotToken ||
                tokens[baseIdx - 1].extraKind == FmtExtraKind::ColonColon) &&
               isIdentifierOrTypeWord(tokens[baseIdx - 2])) {
            baseIdx -= 2;
        }
        const syntax::SyntaxKind beforeKind =
            baseIdx > 0 ? tokens[baseIdx - 1].kind : syntax::SyntaxKind::BadToken;

        bool confirmed = false;
        if (tokens[i - 1].kind == syntax::SyntaxKind::SequenceKeyword ||
            beforeKind == syntax::SyntaxKind::ClassKeyword ||
            beforeKind == syntax::SyntaxKind::StructKeyword ||
            beforeKind == syntax::SyntaxKind::InterfaceKeyword ||
            beforeKind == syntax::SyntaxKind::DelegateKeyword ||
            beforeKind == syntax::SyntaxKind::NewKeyword ||
            beforeKind == syntax::SyntaxKind::IsKeyword ||
            beforeKind == syntax::SyntaxKind::AsKeyword) {
            confirmed = true;
        } else if (baseIdx >= 2 &&
                   tokens[baseIdx - 1].kind == syntax::SyntaxKind::OpenParenToken &&
                   tokens[baseIdx - 2].kind == syntax::SyntaxKind::TypeofKeyword) {
            confirmed = true;
        } else if (closeIdx + 1 < n) {
            const auto& after = tokens[closeIdx + 1];
            if (after.kind == syntax::SyntaxKind::OpenParenToken ||
                after.kind == syntax::SyntaxKind::DotToken ||
                after.extraKind == FmtExtraKind::ColonColon) {
                confirmed = true;
            } else if (after.kind == syntax::SyntaxKind::OpenBracketToken &&
                       closeIdx + 2 < n &&
                       tokens[closeIdx + 2].kind == syntax::SyntaxKind::CloseBracketToken) {
                confirmed = true;
            } else if (after.kind == syntax::SyntaxKind::QuestionToken) {
                // Nullable generic `Box<int>?` when followed by identifier, `[]`, `)`, `,`, `;`, `=`
                if (closeIdx + 2 < n) {
                    const auto nk = tokens[closeIdx + 2].kind;
                    if (nk == syntax::SyntaxKind::IdentifierToken ||
                        nk == syntax::SyntaxKind::OpenBracketToken ||
                        nk == syntax::SyntaxKind::CloseParenToken ||
                        nk == syntax::SyntaxKind::CommaToken ||
                        nk == syntax::SyntaxKind::SemicolonToken) {
                        confirmed = true;
                    }
                }
            } else if (after.kind == syntax::SyntaxKind::OpenBraceToken ||
                       after.kind == syntax::SyntaxKind::ColonToken) {
                confirmed = true;
            } else if (after.kind == syntax::SyntaxKind::CloseParenToken) {
                if (baseIdx > 0 && tokens[baseIdx - 1].kind == syntax::SyntaxKind::OpenParenToken &&
                    closeIdx + 2 < n && startsOperandAfterCast(tokens[closeIdx + 2])) {
                    confirmed = true;
                }
            } else if (after.kind == syntax::SyntaxKind::IdentifierToken) {
                if (after.text == "where") {
                    confirmed = true;
                } else if (!hasTopComma) {
                    confirmed = true;
                } else if (closeIdx + 2 < n) {
                    const auto nk = tokens[closeIdx + 2].kind;
                    if (nk == syntax::SyntaxKind::SemicolonToken ||
                        nk == syntax::SyntaxKind::EqualsToken ||
                        nk == syntax::SyntaxKind::OpenParenToken ||
                        nk == syntax::SyntaxKind::LessToken ||
                        nk == syntax::SyntaxKind::OpenBraceToken ||
                        nk == syntax::SyntaxKind::InKeyword) {
                        confirmed = true;
                    } else if (nk == syntax::SyntaxKind::CommaToken ||
                               nk == syntax::SyntaxKind::CloseParenToken) {
                        // Distinguish parameter declaration `void Foo<T>(Dictionary<string, int> d)` / lambda `(Pair<A, B> x) =>` from call `Foo(a < b, c > d)`
                        for (std::size_t k = i + 1; k < closeIdx; ++k) {
                            if (syntax::isPrimitiveTypeKeyword(tokens[k].kind) ||
                                tokens[k].kind == syntax::SyntaxKind::LessToken ||
                                tokens[k].kind == syntax::SyntaxKind::OpenBracketToken) {
                                confirmed = true;
                                break;
                            }
                        }
                        int pDepth = 0;
                        std::size_t scan = baseIdx;
                        while (!confirmed && scan > 0) {
                            --scan;
                            if (tokens[scan].kind == syntax::SyntaxKind::CloseParenToken) {
                                ++pDepth;
                            } else if (tokens[scan].kind == syntax::SyntaxKind::OpenParenToken) {
                                if (pDepth == 0) {
                                    const std::size_t nameEnd =
                                        (scan >= 1 && tokens[scan - 1].isGenericClose)
                                            ? tokens[scan - 1].matchingIndex
                                            : scan;
                                    if (nameEnd >= 2 &&
                                        tokens[nameEnd - 1].kind == syntax::SyntaxKind::IdentifierToken &&
                                        (isIdentifierOrTypeWord(tokens[nameEnd - 2]) ||
                                         tokens[nameEnd - 2].isGenericClose ||
                                         tokens[nameEnd - 2].kind == syntax::SyntaxKind::CloseBracketToken ||
                                         tokens[nameEnd - 2].kind == syntax::SyntaxKind::QuestionToken ||
                                         syntax::isDeclarationModifier(tokens[nameEnd - 2].kind) ||
                                         tokens[nameEnd - 2].kind == syntax::SyntaxKind::SequenceKeyword ||
                                         tokens[nameEnd - 2].kind == syntax::SyntaxKind::DelegateKeyword)) {
                                        confirmed = true;
                                    } else if (scan >= 1 &&
                                               tokens[scan - 1].kind == syntax::SyntaxKind::CatchKeyword) {
                                        confirmed = true;
                                    } else {
                                        // Check if matching `)` is followed by `=>` (lambda) or `{` / `:` (constructor)
                                        int fwdDepth = 1;
                                        std::size_t fwd = scan + 1;
                                        while (fwd < n && fwdDepth > 0) {
                                            if (tokens[fwd].kind == syntax::SyntaxKind::OpenParenToken) ++fwdDepth;
                                            else if (tokens[fwd].kind == syntax::SyntaxKind::CloseParenToken) --fwdDepth;
                                            ++fwd;
                                        }
                                        if (fwdDepth == 0 && fwd < n &&
                                            (tokens[fwd].kind == syntax::SyntaxKind::ArrowToken ||
                                             tokens[fwd].kind == syntax::SyntaxKind::OpenBraceToken ||
                                             tokens[fwd].kind == syntax::SyntaxKind::ColonToken)) {
                                            confirmed = true;
                                        }
                                    }
                                    break;
                                }
                                --pDepth;
                            }
                        }
                    }
                }
            }
        }

        if (confirmed) {
            markNestedGenerics(markNestedGenerics, i, closeIdx);
            i = closeIdx;
        }
    }
}

bool hasAnyCommentsInRange(
    const std::vector<FmtToken>& tokens,
    std::size_t openIdx,
    std::size_t closeIdx) noexcept {
    for (std::size_t i = openIdx + 1; i <= closeIdx && i < tokens.size(); ++i) {
        if (!tokens[i].leadingComments.empty()) return true;
    }
    return false;
}

void classifyBracketsParensBracesAndOperators(std::vector<FmtToken>& tokens) {
    const std::size_t n = tokens.size();

    // 1. Match `(` `)`, `[` `]`, `{` `}`
    std::vector<std::size_t> parenStack;
    std::vector<std::size_t> bracketStack;
    std::vector<std::size_t> braceStack;
    for (std::size_t i = 0; i < n; ++i) {
        const auto k = tokens[i].kind;
        if (k == syntax::SyntaxKind::OpenParenToken) {
            parenStack.push_back(i);
        } else if (k == syntax::SyntaxKind::CloseParenToken && !parenStack.empty()) {
            const std::size_t openIdx = parenStack.back();
            parenStack.pop_back();
            tokens[openIdx].matchingIndex = i;
            tokens[i].matchingIndex = openIdx;
        } else if (k == syntax::SyntaxKind::OpenBracketToken) {
            bracketStack.push_back(i);
        } else if (k == syntax::SyntaxKind::CloseBracketToken && !bracketStack.empty()) {
            const std::size_t openIdx = bracketStack.back();
            bracketStack.pop_back();
            tokens[openIdx].matchingIndex = i;
            tokens[i].matchingIndex = openIdx;
        } else if (k == syntax::SyntaxKind::OpenBraceToken) {
            braceStack.push_back(i);
        } else if (k == syntax::SyntaxKind::CloseBraceToken && !braceStack.empty()) {
            const std::size_t openIdx = braceStack.back();
            braceStack.pop_back();
            tokens[openIdx].matchingIndex = i;
            tokens[i].matchingIndex = openIdx;
        }
    }

    // 2. Classify Attribute `[...]` vs Array/Element `[...]`
    for (std::size_t i = 0; i < n; ++i) {
        if (tokens[i].kind != syntax::SyntaxKind::OpenBracketToken) continue;
        const std::size_t closeIdx = tokens[i].matchingIndex;
        if (closeIdx <= i || closeIdx >= n) continue;

        bool isAttr = false;
        if (i == 0) {
            isAttr = true;
        } else {
            const auto& prev = tokens[i - 1];
            if (prev.kind == syntax::SyntaxKind::SemicolonToken ||
                prev.kind == syntax::SyntaxKind::OpenBraceToken ||
                prev.kind == syntax::SyntaxKind::CloseBraceToken ||
                prev.isAttributeClose) {
                isAttr = true;
            }
        }
        if (isAttr) {
            tokens[i].isAttributeOpen = true;
            tokens[closeIdx].isAttributeClose = true;
        }
    }

    // 3. Classify `(` ... `)` roles
    for (std::size_t i = 0; i < n; ++i) {
        if (tokens[i].kind != syntax::SyntaxKind::OpenParenToken) continue;
        const std::size_t closeIdx = tokens[i].matchingIndex;
        if (closeIdx <= i || closeIdx >= n) continue;

        ParenRole role = ParenRole::Normal;
        if (closeIdx + 1 < n && tokens[closeIdx + 1].kind == syntax::SyntaxKind::ArrowToken) {
            role = ParenRole::LambdaParams;
        } else if (i > 0) {
            const auto pk = tokens[i - 1].kind;
            if (pk == syntax::SyntaxKind::ForKeyword) {
                role = ParenRole::ForHeader;
            } else if (pk == syntax::SyntaxKind::IfKeyword ||
                       pk == syntax::SyntaxKind::WhileKeyword ||
                       pk == syntax::SyntaxKind::ForeachKeyword ||
                       pk == syntax::SyntaxKind::SwitchKeyword ||
                       pk == syntax::SyntaxKind::CatchKeyword ||
                       (pk == syntax::SyntaxKind::IdentifierToken && tokens[i - 1].text == "using")) {
                role = ParenRole::Control;
            }
        }

        if (role == ParenRole::Normal && closeIdx > i + 1) {
            // Check cast `(Type)expr`
            bool onlyTypeTokens = true;
            for (std::size_t k = i + 1; k < closeIdx; ++k) {
                if (!isAllowedInsideGeneric(tokens, k)) {
                    onlyTypeTokens = false;
                    break;
                }
            }
            if (onlyTypeTokens) {
                const bool startsWithPrim = syntax::isPrimitiveTypeKeyword(tokens[i + 1].kind);
                if (startsWithPrim && closeIdx + 1 < n &&
                    tokens[closeIdx + 1].kind != syntax::SyntaxKind::SemicolonToken &&
                    tokens[closeIdx + 1].kind != syntax::SyntaxKind::CommaToken &&
                    tokens[closeIdx + 1].kind != syntax::SyntaxKind::CloseParenToken) {
                    role = ParenRole::Cast;
                } else if (tokens[i + 1].kind == syntax::SyntaxKind::IdentifierToken &&
                           closeIdx + 1 < n &&
                           startsOperandAfterCast(tokens[closeIdx + 1])) {
                    // Only a cast if `(` was NOT preceded by a callable identifier/keyword!
                    const bool precededByCallable =
                        i > 0 &&
                        (tokens[i - 1].kind == syntax::SyntaxKind::IdentifierToken ||
                         tokens[i - 1].isGenericClose ||
                         tokens[i - 1].kind == syntax::SyntaxKind::TypeofKeyword ||
                         tokens[i - 1].kind == syntax::SyntaxKind::BaseKeyword ||
                         tokens[i - 1].kind == syntax::SyntaxKind::ThisKeyword);
                    if (!precededByCallable) {
                        role = ParenRole::Cast;
                    }
                }
            }
        }

        tokens[i].parenRole = role;
        tokens[closeIdx].parenRole = role;
    }

    // 4. Classify `{` ... `}` roles
    for (std::size_t i = 0; i < n; ++i) {
        if (tokens[i].kind != syntax::SyntaxKind::OpenBraceToken) continue;
        const std::size_t closeIdx = tokens[i].matchingIndex;
        if (closeIdx <= i || closeIdx >= n) continue;

        BraceRole role = BraceRole::Block;
        const bool hasComments = hasAnyCommentsInRange(tokens, i, closeIdx);

        if (closeIdx == i + 1 && !hasComments) {
            role = BraceRole::EmptyBlock;
        } else if (i > 0 && tokens[i - 1].kind == syntax::SyntaxKind::SwitchKeyword) {
            role = BraceRole::SwitchExpression;
        } else if (i > 0 && tokens[i - 1].kind == syntax::SyntaxKind::CloseParenToken) {
            const std::size_t openParen = tokens[i - 1].matchingIndex;
            if (openParen > 0 && tokens[openParen - 1].kind == syntax::SyntaxKind::SwitchKeyword) {
                role = BraceRole::SwitchStatement;
            } else if (openParen > 0) {
                // Check `new Type(...) { ... }` object/collection initializer
                std::size_t scan = openParen - 1;
                while (scan > 0 &&
                       (tokens[scan].isGenericClose || tokens[scan].isGenericOpen ||
                        isAllowedInsideGeneric(tokens, scan))) {
                    if (tokens[scan].isGenericClose) {
                        scan = tokens[scan].matchingIndex;
                    }
                    if (scan == 0) break;
                    --scan;
                }
                if (tokens[scan].kind == syntax::SyntaxKind::NewKeyword && !hasComments) {
                    role = BraceRole::InlineInitializer;
                }
            }
        } else if (i >= 2 && tokens[i - 2].kind == syntax::SyntaxKind::EnumKeyword) {
            role = BraceRole::EnumBody;
        } else if (!hasComments) {
            // Check auto-property `{ get; set; }` or `{ get; }`
            bool isAutoProp = (closeIdx > i + 1);
            for (std::size_t k = i + 1; k < closeIdx; k += 2) {
                if (k + 1 >= closeIdx ||
                    (tokens[k].kind != syntax::SyntaxKind::GetKeyword &&
                     tokens[k].kind != syntax::SyntaxKind::SetKeyword) ||
                    tokens[k + 1].kind != syntax::SyntaxKind::SemicolonToken) {
                    isAutoProp = false;
                    break;
                }
            }
            if (isAutoProp) {
                role = BraceRole::AutoProperty;
            } else if (i > 0) {
                const auto& prev = tokens[i - 1];
                // Array/object initializer: `new int[3] { 1, 2, 3 }`, `int[] a = { 1, 2 }`, `new Obj { x = 1 }`, or nested `{ k, v }`
                bool isInit = false;
                if (prev.kind == syntax::SyntaxKind::EqualsToken ||
                    (prev.kind == syntax::SyntaxKind::CloseBracketToken && !prev.isAttributeClose)) {
                    isInit = true;
                } else if (prev.kind == syntax::SyntaxKind::OpenBraceToken &&
                           prev.braceRole == BraceRole::InlineInitializer) {
                    isInit = true;
                } else if (prev.kind == syntax::SyntaxKind::CommaToken) {
                    // Check enclosing brace
                    int bDepth = 0;
                    for (std::size_t b = i; b > 0; --b) {
                        if (tokens[b - 1].kind == syntax::SyntaxKind::CloseBraceToken) ++bDepth;
                        else if (tokens[b - 1].kind == syntax::SyntaxKind::OpenBraceToken) {
                            if (bDepth == 0) {
                                if (tokens[b - 1].braceRole == BraceRole::InlineInitializer) {
                                    isInit = true;
                                }
                                break;
                            }
                            --bDepth;
                        }
                    }
                } else if (isIdentifierOrTypeWord(prev) || prev.isGenericClose) {
                    // `new Foo { a = 1 }`
                    std::size_t scan = i - 1;
                    if (tokens[scan].isGenericClose) scan = tokens[scan].matchingIndex;
                    while (scan > 0 && isIdentifierOrTypeWord(tokens[scan])) --scan;
                    if (tokens[scan].kind == syntax::SyntaxKind::NewKeyword) {
                        isInit = true;
                    }
                }
                if (isInit) {
                    // Ensure no `;` inside so we never flatten a real block statement
                    bool hasSemi = false;
                    for (std::size_t k = i + 1; k < closeIdx; ++k) {
                        if (tokens[k].kind == syntax::SyntaxKind::SemicolonToken) {
                            hasSemi = true;
                            break;
                        }
                    }
                    if (!hasSemi) {
                        role = BraceRole::InlineInitializer;
                    }
                }
            }
        }

        tokens[i].braceRole = role;
        tokens[closeIdx].braceRole = role;
    }

    // 5. Classify Unary `+`/`-`/`!`, Prefix/Postfix `++`/`--`, `?`, and `:`
    std::vector<std::size_t> activeBraceOpens;
    std::vector<std::size_t> activeParenOpens;
    int ternaryDepth = 0;

    const auto endsExpressionValue = [](const FmtToken& t) noexcept {
        if (t.extraKind == FmtExtraKind::PlusPlus || t.extraKind == FmtExtraKind::MinusMinus) {
            return true;
        }
        if (t.isGenericClose || t.isAttributeClose) return false;
        if (t.kind == syntax::SyntaxKind::CloseParenToken) {
            return t.parenRole != ParenRole::Cast &&
                t.parenRole != ParenRole::Control &&
                t.parenRole != ParenRole::ForHeader &&
                t.parenRole != ParenRole::LambdaParams;
        }
        switch (t.kind) {
        case syntax::SyntaxKind::IdentifierToken:
        case syntax::SyntaxKind::IntegerLiteralToken:
        case syntax::SyntaxKind::FloatLiteralToken:
        case syntax::SyntaxKind::StringLiteralToken:
        case syntax::SyntaxKind::InterpolatedStringToken:
        case syntax::SyntaxKind::TrueKeyword:
        case syntax::SyntaxKind::FalseKeyword:
        case syntax::SyntaxKind::NullKeyword:
        case syntax::SyntaxKind::ThisKeyword:
        case syntax::SyntaxKind::BaseKeyword:
        case syntax::SyntaxKind::CloseBracketToken:
            return true;
        default:
            return false;
        }
    };

    for (std::size_t i = 0; i < n; ++i) {
        auto& tok = tokens[i];
        if (tok.kind == syntax::SyntaxKind::OpenBraceToken) {
            activeBraceOpens.push_back(i);
        } else if (tok.kind == syntax::SyntaxKind::CloseBraceToken && !activeBraceOpens.empty()) {
            activeBraceOpens.pop_back();
        } else if (tok.kind == syntax::SyntaxKind::OpenParenToken) {
            activeParenOpens.push_back(i);
        } else if (tok.kind == syntax::SyntaxKind::CloseParenToken && !activeParenOpens.empty()) {
            activeParenOpens.pop_back();
        }

        if (tok.kind == syntax::SyntaxKind::BangToken) {
            tok.isUnaryPrefix = true;
        } else if (tok.extraKind == FmtExtraKind::PlusPlus || tok.extraKind == FmtExtraKind::MinusMinus) {
            if (i > 0 && endsExpressionValue(tokens[i - 1])) {
                tok.isUnaryPostfix = true;
            } else {
                tok.isUnaryPrefix = true;
            }
        } else if (tok.kind == syntax::SyntaxKind::PlusToken || tok.kind == syntax::SyntaxKind::MinusToken) {
            if (i == 0 || !endsExpressionValue(tokens[i - 1])) {
                tok.isUnaryPrefix = true;
            }
        } else if (tok.kind == syntax::SyntaxKind::QuestionToken) {
            // Nullable `T?` or null-conditional element `?[0]` vs ternary `cond ? a : b`
            bool isNullOrAccess = false;
            if (i + 1 < n && tokens[i + 1].kind == syntax::SyntaxKind::OpenBracketToken) {
                // Both `int?[]` and `items?[0]` attach `?` with no spaces on either side!
                isNullOrAccess = true;
            } else if (i > 0 && (syntax::isPrimitiveTypeKeyword(tokens[i - 1].kind) || tokens[i - 1].isGenericClose)) {
                isNullOrAccess = true;
            } else if (i > 0 && tokens[i - 1].kind == syntax::SyntaxKind::IdentifierToken) {
                // Check if inside generic `<...>` or followed by declaration identifier
                if (i + 1 < n &&
                    (tokens[i + 1].isGenericClose ||
                     tokens[i + 1].kind == syntax::SyntaxKind::IdentifierToken)) {
                    // Wait: in `cond ? x : y`, `tokens[i + 1]` CAN be an identifier `x`!
                    // Scan ahead at same paren/bracket/brace depth for a matching `:` before `;`, `,`, `)`, `]`, `}`, `=>`
                    int p = 0, b = 0, c = 0, q = 0;
                    bool foundTernaryColon = false;
                    for (std::size_t k = i + 1; k < n; ++k) {
                        const auto kk = tokens[k].kind;
                        if (tokens[k].isGenericOpen || tokens[k].isGenericClose) continue;
                        if (kk == syntax::SyntaxKind::OpenParenToken) ++p;
                        else if (kk == syntax::SyntaxKind::CloseParenToken) {
                            if (p == 0) break;
                            --p;
                        } else if (kk == syntax::SyntaxKind::OpenBracketToken) ++b;
                        else if (kk == syntax::SyntaxKind::CloseBracketToken) {
                            if (b == 0) break;
                            --b;
                        } else if (kk == syntax::SyntaxKind::OpenBraceToken) ++c;
                        else if (kk == syntax::SyntaxKind::CloseBraceToken) {
                            if (c == 0) break;
                            --c;
                        } else if (p == 0 && b == 0 && c == 0) {
                            if (kk == syntax::SyntaxKind::SemicolonToken ||
                                kk == syntax::SyntaxKind::CommaToken ||
                                kk == syntax::SyntaxKind::EqualsToken ||
                                kk == syntax::SyntaxKind::ArrowToken) {
                                break;
                            }
                            if (kk == syntax::SyntaxKind::QuestionToken) {
                                ++q;
                            } else if (kk == syntax::SyntaxKind::ColonToken &&
                                       tokens[k].extraKind == FmtExtraKind::None) {
                                if (q == 0) {
                                    foundTernaryColon = true;
                                    break;
                                }
                                --q;
                            }
                        }
                    }
                    if (!foundTernaryColon) {
                        isNullOrAccess = true;
                    }
                }
            }
            tok.isNullableOrCondAccessQuestion = isNullOrAccess;
            if (!isNullOrAccess) {
                ++ternaryDepth;
            }
        } else if (tok.kind == syntax::SyntaxKind::ColonToken && tok.extraKind == FmtExtraKind::None) {
            if (ternaryDepth > 0) {
                tok.colonRole = ColonRole::Spacing;
                --ternaryDepth;
            } else if (!activeBraceOpens.empty() &&
                       tokens[activeBraceOpens.back()].braceRole == BraceRole::SwitchStatement &&
                       activeParenOpens.empty()) {
                tok.colonRole = ColonRole::SwitchLabel;
            } else if (!activeParenOpens.empty() &&
                       tokens[activeParenOpens.back()].parenRole == ParenRole::Normal &&
                       i >= 2 &&
                       tokens[i - 1].kind == syntax::SyntaxKind::IdentifierToken &&
                       (tokens[i - 2].kind == syntax::SyntaxKind::OpenParenToken ||
                        tokens[i - 2].kind == syntax::SyntaxKind::CommaToken)) {
                tok.colonRole = ColonRole::NamedArgument;
            } else {
                tok.colonRole = ColonRole::Spacing;
            }
        }
    }
}

bool isBinaryOrAssignOperator(const FmtToken& tok) noexcept {
    if (tok.extraKind != FmtExtraKind::None) return false;
    if (tok.isGenericOpen || tok.isGenericClose || tok.isUnaryPrefix || tok.isUnaryPostfix) {
        return false;
    }
    switch (tok.kind) {
    case syntax::SyntaxKind::PlusToken:
    case syntax::SyntaxKind::PlusEqualsToken:
    case syntax::SyntaxKind::MinusToken:
    case syntax::SyntaxKind::MinusEqualsToken:
    case syntax::SyntaxKind::ArrowToken:
    case syntax::SyntaxKind::StarToken:
    case syntax::SyntaxKind::StarEqualsToken:
    case syntax::SyntaxKind::SlashToken:
    case syntax::SyntaxKind::SlashEqualsToken:
    case syntax::SyntaxKind::PercentToken:
    case syntax::SyntaxKind::PercentEqualsToken:
    case syntax::SyntaxKind::AmpersandAmpersandToken:
    case syntax::SyntaxKind::PipePipeToken:
    case syntax::SyntaxKind::EqualsToken:
    case syntax::SyntaxKind::EqualsEqualsToken:
    case syntax::SyntaxKind::BangEqualsToken:
    case syntax::SyntaxKind::LessToken:
    case syntax::SyntaxKind::LessOrEqualsToken:
    case syntax::SyntaxKind::GreaterToken:
    case syntax::SyntaxKind::GreaterOrEqualsToken:
    case syntax::SyntaxKind::QuestionQuestionToken:
    case syntax::SyntaxKind::IsKeyword:
    case syntax::SyntaxKind::AsKeyword:
    case syntax::SyntaxKind::InKeyword:
    case syntax::SyntaxKind::WhenKeyword:
        return true;
    case syntax::SyntaxKind::QuestionToken:
        return !tok.isNullableOrCondAccessQuestion;
    case syntax::SyntaxKind::ColonToken:
        return tok.colonRole == ColonRole::Spacing;
    default:
        return false;
    }
}

bool needsSpaceBetween(const FmtToken& prev, const FmtToken& curr) noexcept {
    // 1. Inside / around `::`, `.`, `?.`
    if (prev.extraKind == FmtExtraKind::ColonColon || curr.extraKind == FmtExtraKind::ColonColon) {
        return false;
    }
    if (prev.kind == syntax::SyntaxKind::DotToken || curr.kind == syntax::SyntaxKind::DotToken ||
        prev.kind == syntax::SyntaxKind::QuestionDotToken || curr.kind == syntax::SyntaxKind::QuestionDotToken) {
        return false;
    }

    // 2. Generic `<` and `>`
    if (curr.isGenericOpen) {
        return prev.kind == syntax::SyntaxKind::CaseKeyword ||
            prev.kind == syntax::SyntaxKind::ReturnKeyword;
    }
    if (prev.isGenericOpen || curr.isGenericClose) {
        return false;
    }
    if (prev.isGenericClose) {
        if (curr.kind == syntax::SyntaxKind::OpenParenToken ||
            curr.kind == syntax::SyntaxKind::CloseParenToken ||
            curr.kind == syntax::SyntaxKind::OpenBracketToken ||
            curr.kind == syntax::SyntaxKind::CloseBracketToken ||
            curr.kind == syntax::SyntaxKind::CommaToken ||
            curr.kind == syntax::SyntaxKind::SemicolonToken ||
            (curr.kind == syntax::SyntaxKind::QuestionToken && curr.isNullableOrCondAccessQuestion)) {
            return false;
        }
        return true;
    }

    // 3. Nullable / null-conditional `?[0]` or `T?`
    if (curr.kind == syntax::SyntaxKind::QuestionToken && curr.isNullableOrCondAccessQuestion) {
        return false;
    }
    if (prev.kind == syntax::SyntaxKind::QuestionToken && prev.isNullableOrCondAccessQuestion) {
        if (curr.kind == syntax::SyntaxKind::OpenBracketToken ||
            curr.kind == syntax::SyntaxKind::CloseParenToken ||
            curr.kind == syntax::SyntaxKind::CommaToken ||
            curr.kind == syntax::SyntaxKind::SemicolonToken ||
            curr.isGenericClose) {
            return false;
        }
        return true;
    }

    // 4. Unary prefix & postfix operators
    if (curr.isUnaryPostfix) return false;
    if (prev.isUnaryPrefix) return false;

    // 5. Parentheses `(` and `)`
    if (prev.kind == syntax::SyntaxKind::OpenParenToken ||
        curr.kind == syntax::SyntaxKind::CloseParenToken) {
        return false;
    }
    if (curr.kind == syntax::SyntaxKind::OpenParenToken) {
        if (curr.parenRole == ParenRole::Control || curr.parenRole == ParenRole::ForHeader) {
            return true;
        }
        if (prev.kind == syntax::SyntaxKind::ReturnKeyword ||
            prev.kind == syntax::SyntaxKind::ThrowKeyword ||
            prev.kind == syntax::SyntaxKind::CaseKeyword ||
            prev.kind == syntax::SyntaxKind::WhenKeyword ||
            prev.kind == syntax::SyntaxKind::InKeyword ||
            prev.kind == syntax::SyntaxKind::AwaitKeyword ||
            isBinaryOrAssignOperator(prev) ||
            prev.kind == syntax::SyntaxKind::CommaToken ||
            prev.kind == syntax::SyntaxKind::SemicolonToken ||
            (prev.kind == syntax::SyntaxKind::ColonToken && prev.colonRole != ColonRole::SwitchLabel)) {
            return true;
        }
        return false;
    }
    if (prev.kind == syntax::SyntaxKind::CloseParenToken) {
        if (prev.parenRole == ParenRole::Cast) {
            return false;
        }
        if (curr.kind == syntax::SyntaxKind::SemicolonToken ||
            curr.kind == syntax::SyntaxKind::CommaToken ||
            curr.kind == syntax::SyntaxKind::CloseParenToken ||
            curr.kind == syntax::SyntaxKind::CloseBracketToken ||
            curr.kind == syntax::SyntaxKind::OpenBracketToken) {
            return false;
        }
        return true;
    }

    // 6. Brackets `[` and `]`
    if (prev.kind == syntax::SyntaxKind::OpenBracketToken ||
        curr.kind == syntax::SyntaxKind::CloseBracketToken) {
        return false;
    }
    if (curr.kind == syntax::SyntaxKind::OpenBracketToken) {
        if (curr.isAttributeOpen) {
            return true;
        }
        if (isBinaryOrAssignOperator(prev) ||
            prev.kind == syntax::SyntaxKind::ReturnKeyword ||
            prev.kind == syntax::SyntaxKind::CommaToken) {
            return true;
        }
        return false;
    }
    if (prev.kind == syntax::SyntaxKind::CloseBracketToken) {
        if (prev.isAttributeClose) {
            return true;
        }
        if (curr.kind == syntax::SyntaxKind::OpenBracketToken ||
            curr.kind == syntax::SyntaxKind::OpenParenToken ||
            curr.kind == syntax::SyntaxKind::CloseParenToken ||
            curr.kind == syntax::SyntaxKind::CommaToken ||
            curr.kind == syntax::SyntaxKind::SemicolonToken) {
            return false;
        }
        return true;
    }

    // 7. Braces `{` and `}`
    if (curr.kind == syntax::SyntaxKind::OpenBraceToken) {
        return true;
    }
    if (prev.kind == syntax::SyntaxKind::OpenBraceToken) {
        return prev.braceRole != BraceRole::EmptyBlock;
    }
    if (curr.kind == syntax::SyntaxKind::CloseBraceToken) {
        return curr.braceRole != BraceRole::EmptyBlock;
    }
    if (prev.kind == syntax::SyntaxKind::CloseBraceToken) {
        if (curr.kind == syntax::SyntaxKind::SemicolonToken ||
            curr.kind == syntax::SyntaxKind::CommaToken ||
            curr.kind == syntax::SyntaxKind::CloseParenToken) {
            return false;
        }
        return true;
    }

    // 8. Comma `,`, Semicolon `;`, Colon `:`
    if (curr.kind == syntax::SyntaxKind::CommaToken ||
        curr.kind == syntax::SyntaxKind::SemicolonToken) {
        return false;
    }
    if (prev.kind == syntax::SyntaxKind::CommaToken ||
        prev.kind == syntax::SyntaxKind::SemicolonToken) {
        return true;
    }
    if (curr.kind == syntax::SyntaxKind::ColonToken) {
        return curr.colonRole == ColonRole::Spacing;
    }
    if (prev.kind == syntax::SyntaxKind::ColonToken) {
        return true;
    }

    // 9. Binary & assignment operators
    if (isBinaryOrAssignOperator(prev) || isBinaryOrAssignOperator(curr)) {
        return true;
    }

    // 10. Two adjacent word/literal/string tokens (including unary prefix after keyword/operator)
    if (curr.isUnaryPrefix) {
        return true;
    }
    if (isWordLikeKind(prev.kind) ||
        prev.kind == syntax::SyntaxKind::StringLiteralToken ||
        prev.kind == syntax::SyntaxKind::InterpolatedStringToken ||
        prev.isUnaryPostfix) {
        return true;
    }

    return false;
}

} // namespace

FormatResult formatSource(std::string_view sourceText, const FormatOptions& options) {
    FormatResult result;
    const std::string normalized = normalizeLineEndings(sourceText);

    text::SourceText src(normalized, "<format>");
    diagnostics::DiagnosticBag lexDiags;
    syntax::Lexer lexer(src, lexDiags);
    const auto rawTokens = lexer.lexAll();

    for (const auto& diag : lexDiags.items()) {
        if (diag.code == "RS1000" || diag.code == "RS1001" || diag.code == "RS1002") {
            result.formatted = std::string(sourceText);
            result.changed = false;
            result.unchangedOnError = true;
            result.errorMessage = diag.code + ": " + diag.message;
            return result;
        }
    }

    // Build FmtToken stream with attached trivia and fused `++`, `--`, `::`
    std::vector<FmtToken> tokens;
    tokens.reserve(rawTokens.size());
    std::size_t prevEnd = 0;
    for (std::size_t i = 0; i < rawTokens.size(); ++i) {
        const auto& rt = rawTokens[i];
        FmtToken ft;
        ft.kind = rt.kind;
        ft.span = rt.span;
        ft.text = rt.text;
        const std::size_t triviaLen = rt.span.start >= prevEnd ? (rt.span.start - prevEnd) : 0;
        parseTriviaSlice(
            std::string_view(normalized).substr(prevEnd, triviaLen),
            ft.leadingComments,
            ft.newlinesAfterComments);
        prevEnd = rt.span.end();

        // Fuse adjacent `++` / `--` and comment-free `::`
        if (i + 1 < rawTokens.size()) {
            const auto& nextRt = rawTokens[i + 1];
            if (rt.span.end() == nextRt.span.start) {
                if (rt.kind == syntax::SyntaxKind::PlusToken &&
                    nextRt.kind == syntax::SyntaxKind::PlusToken) {
                    ft.extraKind = FmtExtraKind::PlusPlus;
                    ft.text = "++";
                    ft.span = text::TextSpan{rt.span.start, 2};
                    prevEnd = nextRt.span.end();
                    ++i;
                } else if (rt.kind == syntax::SyntaxKind::MinusToken &&
                           nextRt.kind == syntax::SyntaxKind::MinusToken) {
                    ft.extraKind = FmtExtraKind::MinusMinus;
                    ft.text = "--";
                    ft.span = text::TextSpan{rt.span.start, 2};
                    prevEnd = nextRt.span.end();
                    ++i;
                }
            }
            if (ft.extraKind == FmtExtraKind::None &&
                rt.kind == syntax::SyntaxKind::ColonToken &&
                nextRt.kind == syntax::SyntaxKind::ColonToken) {
                std::vector<TriviaComment> midComments;
                std::size_t midNl = 0;
                parseTriviaSlice(
                    std::string_view(normalized).substr(
                        rt.span.end(),
                        nextRt.span.start >= rt.span.end() ? (nextRt.span.start - rt.span.end()) : 0),
                    midComments,
                    midNl);
                if (midComments.empty()) {
                    ft.extraKind = FmtExtraKind::ColonColon;
                    ft.text = "::";
                    ft.span = text::TextSpan{rt.span.start, nextRt.span.end() - rt.span.start};
                    prevEnd = nextRt.span.end();
                    ++i;
                }
            }
        }
        tokens.push_back(std::move(ft));
    }

    classifyGenerics(tokens);
    classifyBracketsParensBracesAndOperators(tokens);

    // Formatting writer state
    std::string out;
    out.reserve(normalized.size() + 128);
    bool atLineStart = true;
    std::size_t consecutiveBlankLines = 0;
    bool justOpenedBlockBrace = false;

    struct ActiveBraceFrame {
        BraceRole role = BraceRole::Block;
        bool inSwitchCaseBody = false;
        int savedBracelessIndent = 0;
    };
    std::vector<ActiveBraceFrame> braceFrames;
    std::vector<ParenRole> parenFrames;
    int bracelessIndent = 0;
    bool pendingDoWhile = false;
    std::vector<int> doWhileBraceDepths;

    const auto currentIndentDepth = [&](bool forClosingBrace = false, bool forSwitchLabel = false) -> int {
        int depth = 0;
        const std::size_t limit = (forClosingBrace && !braceFrames.empty())
            ? (braceFrames.size() - 1)
            : braceFrames.size();
        for (std::size_t bi = 0; bi < limit; ++bi) {
            const auto& bf = braceFrames[bi];
            if (bf.role == BraceRole::Block ||
                bf.role == BraceRole::SwitchExpression ||
                bf.role == BraceRole::EnumBody) {
                ++depth;
            } else if (bf.role == BraceRole::SwitchStatement) {
                ++depth; // `case` / `default` level
                const bool isInnerMostSwitch = (bi + 1 == limit);
                if (bf.inSwitchCaseBody && !(forSwitchLabel && isInnerMostSwitch)) {
                    ++depth;
                }
            }
        }
        if (!forClosingBrace && !forSwitchLabel) {
            depth += bracelessIndent;
        }
        return std::max(0, depth);
    };

    const auto emitNewline = [&]() {
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
            out.pop_back();
        }
        if (out.empty()) {
            atLineStart = true;
            return;
        }
        if (atLineStart) {
            if (!justOpenedBlockBrace && consecutiveBlankLines < options.maxConsecutiveBlankLines) {
                out.push_back('\n');
                ++consecutiveBlankLines;
            }
        } else {
            out.push_back('\n');
            atLineStart = true;
            consecutiveBlankLines = 0;
        }
    };

    const auto emitBlankLinesFromCount = [&](std::size_t rawNewlineCount) {
        if (rawNewlineCount == 0 || out.empty()) return;
        if (!atLineStart) {
            emitNewline();
        }
        const std::size_t desiredBlanks = rawNewlineCount > 1 ? (rawNewlineCount - 1) : 0;
        const std::size_t allowedBlanks = justOpenedBlockBrace
            ? 0
            : std::min(desiredBlanks, options.maxConsecutiveBlankLines);
        while (consecutiveBlankLines < allowedBlanks) {
            out.push_back('\n');
            ++consecutiveBlankLines;
        }
    };

    const auto ensureIndented = [&](int indentDepth) {
        if (atLineStart) {
            out.append(
                static_cast<std::size_t>(std::max(0, indentDepth)) * options.indentWidth,
                ' ');
            atLineStart = false;
            consecutiveBlankLines = 0;
        }
    };

    const auto stripBlankLinesBeforeCloseBrace = [&]() {
        // Never leave blank empty lines right before `}`
        while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') {
            out.pop_back();
        }
        consecutiveBlankLines = 0;
    };

    const FmtToken* prevEmittedTok = nullptr;
    bool lastWasInlineComment = false;

    for (std::size_t idx = 0; idx < tokens.size(); ++idx) {
        const auto& tok = tokens[idx];
        const bool isEof = (tok.kind == syntax::SyntaxKind::EndOfFileToken);
        const bool isCloseBrace = (tok.kind == syntax::SyntaxKind::CloseBraceToken);
        const bool isSwitchLabelStart =
            !braceFrames.empty() &&
            braceFrames.back().role == BraceRole::SwitchStatement &&
            parenFrames.empty() &&
            (tok.kind == syntax::SyntaxKind::CaseKeyword ||
             tok.kind == syntax::SyntaxKind::DefaultKeyword);

        // Emit leading comments attached to `tok`
        for (std::size_t ci = 0; ci < tok.leadingComments.size(); ++ci) {
            const auto& cm = tok.leadingComments[ci];
            if (cm.newlinesBefore > 0) {
                emitBlankLinesFromCount(cm.newlinesBefore);
            }
            const bool sameLineTrailing = (cm.newlinesBefore == 0 && prevEmittedTok != nullptr && !atLineStart);
            if (sameLineTrailing) {
                if (!out.empty() && out.back() != ' ') {
                    out.push_back(' ');
                }
                out.append(cm.text);
                if (cm.isLineComment) {
                    justOpenedBlockBrace = false;
                    emitNewline();
                    lastWasInlineComment = false;
                } else {
                    lastWasInlineComment = true;
                }
            } else {
                if (!atLineStart) {
                    emitNewline();
                }
                const int cmIndent = isEof ? currentIndentDepth(false, false) : currentIndentDepth(false, false);
                ensureIndented(cmIndent);
                out.append(cm.text);
                justOpenedBlockBrace = false;
                if (cm.isLineComment) {
                    emitNewline();
                    lastWasInlineComment = false;
                } else {
                    lastWasInlineComment = true;
                }
            }
        }

        if (tok.newlinesAfterComments > 0) {
            emitBlankLinesFromCount(tok.newlinesAfterComments);
            lastWasInlineComment = false;
        }

        if (isEof) {
            break;
        }

        // Structural pre-token line break adjustments
        if (isCloseBrace) {
            const auto role = tok.braceRole;
            if (role == BraceRole::Block ||
                role == BraceRole::SwitchStatement ||
                role == BraceRole::SwitchExpression ||
                role == BraceRole::EnumBody) {
                if (!atLineStart) {
                    justOpenedBlockBrace = false;
                    emitNewline();
                }
                stripBlankLinesBeforeCloseBrace();
            }
        } else if (isSwitchLabelStart) {
            if (!atLineStart) {
                justOpenedBlockBrace = false;
                emitNewline();
            }
        }

        // Compute effective indentation if we are at the start of a line
        if (atLineStart) {
            const int indent = currentIndentDepth(isCloseBrace, isSwitchLabelStart);
            ensureIndented(indent);
        } else if (lastWasInlineComment) {
            if (!out.empty() && out.back() != ' ' &&
                tok.kind != syntax::SyntaxKind::SemicolonToken &&
                tok.kind != syntax::SyntaxKind::CommaToken &&
                tok.kind != syntax::SyntaxKind::CloseParenToken) {
                out.push_back(' ');
            }
        } else if (prevEmittedTok != nullptr) {
            if (needsSpaceBetween(*prevEmittedTok, tok)) {
                out.push_back(' ');
            }
        }

        out.append(tok.text);
        prevEmittedTok = &tok;
        lastWasInlineComment = false;
        justOpenedBlockBrace = false;

        // Track `do` keyword for `do { ... } while (...);`
        if (tok.kind == syntax::SyntaxKind::DoKeyword) {
            pendingDoWhile = true;
        }

        // Update paren / brace / control-flow state & post-token line breaks
        if (tok.kind == syntax::SyntaxKind::OpenParenToken) {
            parenFrames.push_back(tok.parenRole);
        } else if (tok.kind == syntax::SyntaxKind::CloseParenToken) {
            const ParenRole closedRole = parenFrames.empty() ? ParenRole::Normal : parenFrames.back();
            if (!parenFrames.empty()) parenFrames.pop_back();

            // Check braceless control-flow body: `if (...) stmt;` / `for (...) stmt;` / `while (...) stmt;` (not `do { } while (...);`)
            const bool isDoWhileCond =
                !doWhileBraceDepths.empty() &&
                doWhileBraceDepths.back() == static_cast<int>(braceFrames.size()) &&
                idx + 1 < tokens.size() &&
                tokens[idx + 1].kind == syntax::SyntaxKind::SemicolonToken;
            if (isDoWhileCond) {
                doWhileBraceDepths.pop_back();
            } else if ((closedRole == ParenRole::Control || closedRole == ParenRole::ForHeader) &&
                       idx + 1 < tokens.size() &&
                       tokens[idx + 1].kind != syntax::SyntaxKind::OpenBraceToken &&
                       tokens[idx + 1].kind != syntax::SyntaxKind::SemicolonToken) {
                // Only emit newline if there is no same-line trailing comment attached to `)`
                const bool hasTrailingCommentOnSameLine =
                    !tokens[idx + 1].leadingComments.empty() &&
                    tokens[idx + 1].leadingComments.front().newlinesBefore == 0;
                ++bracelessIndent;
                if (!hasTrailingCommentOnSameLine) {
                    emitNewline();
                }
            }
        } else if (tok.kind == syntax::SyntaxKind::ElseKeyword) {
            if (idx + 1 < tokens.size() &&
                tokens[idx + 1].kind != syntax::SyntaxKind::IfKeyword &&
                tokens[idx + 1].kind != syntax::SyntaxKind::OpenBraceToken) {
                const bool hasTrailingCommentOnSameLine =
                    !tokens[idx + 1].leadingComments.empty() &&
                    tokens[idx + 1].leadingComments.front().newlinesBefore == 0;
                ++bracelessIndent;
                if (!hasTrailingCommentOnSameLine) {
                    emitNewline();
                }
            }
        } else if (tok.kind == syntax::SyntaxKind::OpenBraceToken) {
            if (pendingDoWhile) {
                doWhileBraceDepths.push_back(static_cast<int>(braceFrames.size()));
                pendingDoWhile = false;
            }
            ActiveBraceFrame frame;
            frame.role = tok.braceRole;
            frame.inSwitchCaseBody = false;
            frame.savedBracelessIndent = bracelessIndent;
            bracelessIndent = 0;
            braceFrames.push_back(frame);

            if (tok.braceRole == BraceRole::Block ||
                tok.braceRole == BraceRole::SwitchStatement ||
                tok.braceRole == BraceRole::SwitchExpression ||
                tok.braceRole == BraceRole::EnumBody) {
                const bool hasSameLineComment =
                    idx + 1 < tokens.size() &&
                    !tokens[idx + 1].leadingComments.empty() &&
                    tokens[idx + 1].leadingComments.front().newlinesBefore == 0;
                if (!hasSameLineComment) {
                    emitNewline();
                    justOpenedBlockBrace = true;
                }
            }
        } else if (tok.kind == syntax::SyntaxKind::CloseBraceToken) {
            const BraceRole closedRole = braceFrames.empty() ? BraceRole::Block : braceFrames.back().role;
            if (!braceFrames.empty()) {
                bracelessIndent = braceFrames.back().savedBracelessIndent;
                braceFrames.pop_back();
            }

            if (closedRole == BraceRole::Block ||
                closedRole == BraceRole::EmptyBlock ||
                closedRole == BraceRole::SwitchStatement ||
                closedRole == BraceRole::EnumBody ||
                closedRole == BraceRole::AutoProperty) {
                const syntax::SyntaxKind nextK =
                    (idx + 1 < tokens.size()) ? tokens[idx + 1].kind : syntax::SyntaxKind::EndOfFileToken;
                const bool isDoWhileTail =
                    nextK == syntax::SyntaxKind::WhileKeyword &&
                    !doWhileBraceDepths.empty() &&
                    doWhileBraceDepths.back() == static_cast<int>(braceFrames.size());
                const bool stayInline =
                    nextK == syntax::SyntaxKind::ElseKeyword ||
                    nextK == syntax::SyntaxKind::CatchKeyword ||
                    nextK == syntax::SyntaxKind::FinallyKeyword ||
                    isDoWhileTail ||
                    nextK == syntax::SyntaxKind::SemicolonToken ||
                    nextK == syntax::SyntaxKind::CommaToken ||
                    nextK == syntax::SyntaxKind::CloseParenToken ||
                    (closedRole == BraceRole::AutoProperty && nextK == syntax::SyntaxKind::EqualsToken);
                const bool hasSameLineComment =
                    idx + 1 < tokens.size() &&
                    !tokens[idx + 1].leadingComments.empty() &&
                    tokens[idx + 1].leadingComments.front().newlinesBefore == 0;
                if (!stayInline && !hasSameLineComment && nextK != syntax::SyntaxKind::EndOfFileToken) {
                    if (bracelessIndent > 0 && nextK != syntax::SyntaxKind::ElseKeyword) {
                        bracelessIndent = 0;
                    }
                    emitNewline();
                }
            }
        } else if (tok.kind == syntax::SyntaxKind::ColonToken && tok.colonRole == ColonRole::SwitchLabel) {
            if (!braceFrames.empty() && braceFrames.back().role == BraceRole::SwitchStatement) {
                braceFrames.back().inSwitchCaseBody = true;
            }
            const bool hasSameLineComment =
                idx + 1 < tokens.size() &&
                !tokens[idx + 1].leadingComments.empty() &&
                tokens[idx + 1].leadingComments.front().newlinesBefore == 0;
            if (!hasSameLineComment) {
                emitNewline();
            }
        } else if (tok.isAttributeClose) {
            // Put declaration-level attributes on their own line before top-level or class/struct/interface/enum members
            const bool atDeclLevel =
                parenFrames.empty() &&
                (braceFrames.empty() ||
                 braceFrames.back().role == BraceRole::Block ||
                 braceFrames.back().role == BraceRole::EnumBody);
            const bool hasSameLineComment =
                idx + 1 < tokens.size() &&
                !tokens[idx + 1].leadingComments.empty() &&
                tokens[idx + 1].leadingComments.front().newlinesBefore == 0;
            if (atDeclLevel && !hasSameLineComment && idx + 1 < tokens.size() &&
                tokens[idx + 1].kind != syntax::SyntaxKind::EndOfFileToken) {
                emitNewline();
            }
        } else if (tok.kind == syntax::SyntaxKind::CommaToken) {
            // Break lines after comma at top level of SwitchExpression or EnumBody
            const bool inMultiLineCommaBody =
                parenFrames.empty() &&
                !braceFrames.empty() &&
                (braceFrames.back().role == BraceRole::SwitchExpression ||
                 braceFrames.back().role == BraceRole::EnumBody);
            const bool hasSameLineComment =
                idx + 1 < tokens.size() &&
                !tokens[idx + 1].leadingComments.empty() &&
                tokens[idx + 1].leadingComments.front().newlinesBefore == 0;
            if (inMultiLineCommaBody && !hasSameLineComment) {
                emitNewline();
            }
        } else if (tok.kind == syntax::SyntaxKind::SemicolonToken) {
            const bool insideForHeader =
                !parenFrames.empty() && parenFrames.back() == ParenRole::ForHeader;
            const bool insideAutoProp =
                !braceFrames.empty() && braceFrames.back().role == BraceRole::AutoProperty;
            if (!insideForHeader && !insideAutoProp) {
                if (bracelessIndent > 0) {
                    const bool nextIsElse =
                        idx + 1 < tokens.size() &&
                        tokens[idx + 1].kind == syntax::SyntaxKind::ElseKeyword;
                    if (nextIsElse) {
                        --bracelessIndent;
                    } else {
                        bracelessIndent = 0;
                    }
                }
                const bool hasSameLineComment =
                    idx + 1 < tokens.size() &&
                    !tokens[idx + 1].leadingComments.empty() &&
                    tokens[idx + 1].leadingComments.front().newlinesBefore == 0;
                if (!hasSameLineComment && idx + 1 < tokens.size() &&
                    tokens[idx + 1].kind != syntax::SyntaxKind::EndOfFileToken) {
                    emitNewline();
                }
            }
        }
    }

    // Trim trailing blank lines at EOF and apply `insertFinalNewline`
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    if (!out.empty() && options.insertFinalNewline) {
        out.push_back('\n');
    }

    result.changed = (out != sourceText);
    result.formatted = std::move(out);
    return result;
}

} // namespace realscript::tooling
