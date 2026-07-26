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

    const auto span = text::TextSpan{start, position_ - start};
    const auto tokenText = std::string(source_.view(span));
    if (isFloat) {
        char* end = nullptr;
        const double value = std::strtod(tokenText.c_str(), &end);
        if (end == tokenText.c_str() || *end != '\0') {
            diagnostics_.report("RS1003", "invalid floating-point literal", span);
            return {SyntaxKind::FloatLiteralToken, span, tokenText, 0.0, false};
        }
        return {SyntaxKind::FloatLiteralToken, span, tokenText, value, false};
    }

    std::int64_t value = 0;
    const auto result = std::from_chars(tokenText.data(), tokenText.data() + tokenText.size(), value);
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

SyntaxToken Lexer::nextToken() {
    skipTrivia();
    const auto start = position_;

    if (current() == '\0') {
        return {SyntaxKind::EndOfFileToken, {position_, 0}, "", {}, false};
    }
    if (current() >= '0' && current() <= '9') {
        return lexNumber();
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
    case '+': return single(SyntaxKind::PlusToken);
    case '-': return single(SyntaxKind::MinusToken);
    case '*': return single(SyntaxKind::StarToken);
    case '/': return single(SyntaxKind::SlashToken);
    case '%': return single(SyntaxKind::PercentToken);
    case '(': return single(SyntaxKind::OpenParenToken);
    case ')': return single(SyntaxKind::CloseParenToken);
    case '{': return single(SyntaxKind::OpenBraceToken);
    case '}': return single(SyntaxKind::CloseBraceToken);
    case '[': return single(SyntaxKind::OpenBracketToken);
    case ']': return single(SyntaxKind::CloseBracketToken);
    case ',': return single(SyntaxKind::CommaToken);
    case '.': return single(SyntaxKind::DotToken);
    case ':': return single(SyntaxKind::ColonToken);
    case ';': return single(SyntaxKind::SemicolonToken);
    case '!': return peek(1) == '=' ? pair(SyntaxKind::BangEqualsToken) : single(SyntaxKind::BangToken);
    case '=': return peek(1) == '=' ? pair(SyntaxKind::EqualsEqualsToken) : single(SyntaxKind::EqualsToken);
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
