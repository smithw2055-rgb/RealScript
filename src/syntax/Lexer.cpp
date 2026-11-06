#include "realscript/syntax/Syntax.h"

#include <charconv>
#include <cstdlib>

namespace realscript::syntax {
Lexer::Lexer(const text::SourceText& source, diagnostics::DiagnosticBag& diagnostics)
    : source_(source), diagnostics_(diagnostics) {}

char Lexer::current() const noexcept {
    return source_.at(position_);
}

char Lexer::peek(std::size_t offset) const noexcept {
    return source_.at(position_ + offset);
}

void Lexer::advance(std::size_t count) noexcept {
    position_ += count;
}

void Lexer::skipTrivia() {
    for (;;) {
        if (current() == ' ' || current() == '\t' || current() == '\r' || current() == '\n') {
            advance();
            continue;
        }
        if (current() == '/' && peek(1) == '/') {
            advance(2);
            while (current() != '\0' && current() != '\r' && current() != '\n') {
                advance();
            }
            continue;
        }
        if (current() == '/' && peek(1) == '*') {
            const auto start = position_;
            advance(2);
            while (current() != '\0' && !(current() == '*' && peek(1) == '/')) {
                advance();
            }
            if (current() == '\0') {
                diagnostics_.report("RS1002", "unterminated block comment", {start, position_ - start});
                return;
            }
            advance(2);
            continue;
        }
        return;
    }
}

SyntaxToken Lexer::lexNumber() {
    const auto start = position_;
    bool isFloat = false;
    while (current() >= '0' && current() <= '9') {
        advance();
    }
    if (current() == '.' && peek(1) >= '0' && peek(1) <= '9') {
        isFloat = true;
        advance();
        while (current() >= '0' && current() <= '9') {
            advance();
        }
    }
    const auto digitsSpan = text::TextSpan{start, position_ - start};
    const auto digitsText = std::string(source_.view(digitsSpan));
    if (current() == 'f' || current() == 'F' || current() == 'd' || current() == 'D') {
        isFloat = true;
        advance();
    } else if (!isFloat) {
        if (current() == 'u' || current() == 'U') {
            advance();
            if (current() == 'l' || current() == 'L') {
                advance();
            }
        } else if (current() == 'l' || current() == 'L') {
            advance();
            if (current() == 'u' || current() == 'U') {
                advance();
            }
        }
    }

    const auto span = text::TextSpan{start, position_ - start};
    const auto tokenText = std::string(source_.view(span));
    if (isFloat) {
        char* end = nullptr;
        const double value = std::strtod(digitsText.c_str(), &end);
        if (end == digitsText.c_str() || *end != '\0') {
            diagnostics_.report("RS1003", "invalid floating-point literal", span);
            return {SyntaxKind::FloatLiteralToken, span, tokenText, 0.0, false};
        }
        return {SyntaxKind::FloatLiteralToken, span, tokenText, value, false};
    }

    std::int64_t value = 0;
    const auto result = std::from_chars(digitsText.data(), digitsText.data() + digitsText.size(), value);
    if (result.ec != std::errc{}) {
        diagnostics_.report("RS1004", "integer literal is outside the supported range", span);
        value = 0;
    }
    return {SyntaxKind::IntegerLiteralToken, span, tokenText, value, false};
}

SyntaxToken Lexer::lexIdentifierOrKeyword() {
    const auto start = position_;
    while ((current() >= 'a' && current() <= 'z') ||
           (current() >= 'A' && current() <= 'Z') ||
           (current() >= '0' && current() <= '9') || current() == '_') {
        advance();
    }
    const auto span = text::TextSpan{start, position_ - start};
    auto tokenText = std::string(source_.view(span));
    const auto kind = keywordKind(tokenText);
    TokenValue value;
    if (kind == SyntaxKind::TrueKeyword) {
        value = true;
    } else if (kind == SyntaxKind::FalseKeyword) {
        value = false;
    }
    return {kind, span, std::move(tokenText), std::move(value), false};
}

SyntaxToken Lexer::lexString() {
    const auto start = position_;
    advance();
    std::string value;
    bool terminated = false;

    while (current() != '\0') {
        if (current() == '"') {
            advance();
            terminated = true;
            break;
        }
        if (current() == '\r' || current() == '\n') {
            break;
        }
        if (current() == '\\') {
            const char escaped = peek(1);
            switch (escaped) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '\\': value.push_back('\\'); break;
            case '"': value.push_back('"'); break;
            default:
                diagnostics_.report("RS1005", "unsupported escape sequence", {position_, 2});
                value.push_back(escaped);
                break;
            }
            advance(2);
            continue;
        }
        value.push_back(current());
        advance();
    }

    const auto span = text::TextSpan{start, position_ - start};
    if (!terminated) {
        diagnostics_.report("RS1001", "unterminated string literal", span);
    }
    return {SyntaxKind::StringLiteralToken, span, std::string(source_.view(span)), std::move(value), false};
}

SyntaxToken Lexer::lexInterpolatedString() {
    const auto start = position_;
    advance(2); // skip $"
    bool terminated = false;

    enum class ModeKind { InterpText, InterpHole, NormalString };
    struct Mode {
        ModeKind kind;
        int braceDepth = 0;
    };
    std::vector<Mode> stack{{ModeKind::InterpText, 0}};

    while (current() != '\0' && !stack.empty()) {
        const auto mode = stack.back().kind;
        if (mode == ModeKind::InterpText) {
            if (current() == '"') {
                advance();
                stack.pop_back();
                if (stack.empty()) {
                    terminated = true;
                    break;
                }
                continue;
            }
            if (current() == '\r' || current() == '\n') {
                break;
            }
            if (current() == '\\') {
                const char escaped = peek(1);
                if (escaped == '\0') {
                    advance();
                    break;
                }
                if (stack.size() == 1 &&
                    escaped != 'n' && escaped != 'r' && escaped != 't' &&
                    escaped != '\\' && escaped != '"' &&
                    escaped != '{' && escaped != '}') {
                    diagnostics_.report("RS1005", "unsupported escape sequence", {position_, 2});
                }
                advance(2);
                continue;
            }
            if (current() == '{') {
                if (peek(1) == '{') {
                    advance(2);
                    continue;
                }
                stack.push_back({ModeKind::InterpHole, 1});
                advance();
                continue;
            }
            if (current() == '}' && peek(1) == '}') {
                advance(2);
                continue;
            }
            advance();
        } else if (mode == ModeKind::NormalString) {
            if (current() == '\\' && peek(1) != '\0') {
                advance(2);
                continue;
            }
            if (current() == '"') {
                stack.pop_back();
            }
            advance();
        } else {
            if (current() == '$' && peek(1) == '"') {
                advance(2);
                stack.push_back({ModeKind::InterpText, 0});
                continue;
            }
            if (current() == '"') {
                advance();
                stack.push_back({ModeKind::NormalString, 0});
                continue;
            }
            if (current() == '\'') {
                advance();
                if (current() == '\\' && peek(1) != '\0') {
                    advance(2);
                } else if (current() != '\0') {
                    advance();
                }
                if (current() == '\'') {
                    advance();
                }
                continue;
            }
            if (current() == '{') {
                ++stack.back().braceDepth;
                advance();
                continue;
            }
            if (current() == '}') {
                --stack.back().braceDepth;
                advance();
                if (stack.back().braceDepth == 0) {
                    stack.pop_back();
                }
                continue;
            }
            advance();
        }
    }

    const auto span = text::TextSpan{start, position_ - start};
    if (!terminated) {
        diagnostics_.report("RS1001", "unterminated string literal", span);
    }
    return {SyntaxKind::InterpolatedStringToken, span, std::string(source_.view(span)), {}, false};
}

SyntaxToken Lexer::nextToken() {
    skipTrivia();
    const auto start = position_;

    if (current() == '\0') {
        return {SyntaxKind::EndOfFileToken, {position_, 0}, "", {}, false};
    }
    if (current() >= '0' && current() <= '9') {
        return lexNumber();
    }
    if (current() == '$' && peek(1) == '"') {
        return lexInterpolatedString();
    }
    if ((current() >= 'a' && current() <= 'z') ||
        (current() >= 'A' && current() <= 'Z') || current() == '_') {
        return lexIdentifierOrKeyword();
    }
    if (current() == '"') {
        return lexString();
    }

    auto single = [&](SyntaxKind kind) {
        advance();
        const auto span = text::TextSpan{start, 1};
        return SyntaxToken{kind, span, std::string(source_.view(span)), {}, false};
    };
    auto pair = [&](SyntaxKind kind) {
        advance(2);
        const auto span = text::TextSpan{start, 2};
        return SyntaxToken{kind, span, std::string(source_.view(span)), {}, false};
    };

    switch (current()) {
    case '+': return peek(1) == '='
        ? pair(SyntaxKind::PlusEqualsToken)
        : single(SyntaxKind::PlusToken);
    case '-': return peek(1) == '='
        ? pair(SyntaxKind::MinusEqualsToken)
        : single(SyntaxKind::MinusToken);
    case '*': return peek(1) == '='
        ? pair(SyntaxKind::StarEqualsToken)
        : single(SyntaxKind::StarToken);
    case '/': return peek(1) == '='
        ? pair(SyntaxKind::SlashEqualsToken)
        : single(SyntaxKind::SlashToken);
    case '%': return peek(1) == '='
        ? pair(SyntaxKind::PercentEqualsToken)
        : single(SyntaxKind::PercentToken);
    case '(': return single(SyntaxKind::OpenParenToken);
    case ')': return single(SyntaxKind::CloseParenToken);
    case '{': return single(SyntaxKind::OpenBraceToken);
    case '}': return single(SyntaxKind::CloseBraceToken);
    case '[': return single(SyntaxKind::OpenBracketToken);
    case ']': return single(SyntaxKind::CloseBracketToken);
    case ',': return single(SyntaxKind::CommaToken);
    case '.': return single(SyntaxKind::DotToken);
    case ':': return single(SyntaxKind::ColonToken);
    case '?':
        if (peek(1) == '?') return pair(SyntaxKind::QuestionQuestionToken);
        if (peek(1) == '.') return pair(SyntaxKind::QuestionDotToken);
        return single(SyntaxKind::QuestionToken);
    case ';': return single(SyntaxKind::SemicolonToken);
    case '!': return peek(1) == '=' ? pair(SyntaxKind::BangEqualsToken) : single(SyntaxKind::BangToken);
    case '=':
        if (peek(1) == '=') return pair(SyntaxKind::EqualsEqualsToken);
        if (peek(1) == '>') return pair(SyntaxKind::ArrowToken);
        return single(SyntaxKind::EqualsToken);
    case '<': return peek(1) == '=' ? pair(SyntaxKind::LessOrEqualsToken) : single(SyntaxKind::LessToken);
    case '>': return peek(1) == '=' ? pair(SyntaxKind::GreaterOrEqualsToken) : single(SyntaxKind::GreaterToken);
    case '&':
        if (peek(1) == '&') return pair(SyntaxKind::AmpersandAmpersandToken);
        break;
    case '|':
        if (peek(1) == '|') return pair(SyntaxKind::PipePipeToken);
        break;
    default:
        break;
    }

    advance();
    const auto span = text::TextSpan{start, 1};
    diagnostics_.report(
        "RS1000",
        "invalid character '" + std::string(source_.view(span)) + "'",
        span);
    return {SyntaxKind::BadToken, span, std::string(source_.view(span)), {}, false};
}

std::vector<SyntaxToken> Lexer::lexAll() {
    std::vector<SyntaxToken> result;
    for (;;) {
        auto token = nextToken();
        if (token.kind != SyntaxKind::BadToken) {
            result.push_back(token);
        }
        if (token.kind == SyntaxKind::EndOfFileToken) {
            break;
        }
    }
    return result;
}


} // namespace realscript::syntax
