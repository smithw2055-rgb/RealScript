enum class TokenKind { Identifier, Number, String, Symbol, End };

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    std::size_t offset = 0;
};

bool isIdentifierStart(char value) {
    return std::isalpha(static_cast<unsigned char>(value)) || value == '_';
}

bool isIdentifierPart(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

std::vector<Token> lex(const std::string& source) {
    std::vector<Token> tokens;
    std::size_t position = 0;
    while (position < source.size()) {
        const auto current = source[position];
        if (std::isspace(static_cast<unsigned char>(current))) {
            ++position;
            continue;
        }
        if (current == '/' && position + 1 < source.size() && source[position + 1] == '/') {
            position += 2;
            while (position < source.size() && source[position] != '\n') ++position;
            continue;
        }
        if (current == '/' && position + 1 < source.size() && source[position + 1] == '*') {
            const auto start = position;
            position += 2;
            int depth = 1;
            while (position < source.size() && depth != 0) {
                if (position + 1 < source.size() && source[position] == '/' && source[position + 1] == '*') {
                    ++depth;
                    position += 2;
                } else if (position + 1 < source.size() && source[position] == '*' && source[position + 1] == '/') {
                    --depth;
                    position += 2;
                } else {
                    ++position;
                }
            }
            if (depth != 0) tokens.push_back({TokenKind::Symbol, "/*", start});
            continue;
        }
        if (isIdentifierStart(current)) {
            const auto start = position++;
            while (position < source.size() && isIdentifierPart(source[position])) ++position;
            tokens.push_back({TokenKind::Identifier, source.substr(start, position - start), start});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(current))) {
            const auto start = position++;
            while (position < source.size()) {
                const auto value = source[position];
                if (!std::isalnum(static_cast<unsigned char>(value)) && value != '.' && value != '_') break;
                ++position;
            }
            tokens.push_back({TokenKind::Number, source.substr(start, position - start), start});
            continue;
        }
        if (current == '"' || current == '\'') {
            const auto quote = current;
            const auto start = position++;
            bool escaped = false;
            while (position < source.size()) {
                const auto value = source[position++];
                if (escaped) {
                    escaped = false;
                } else if (value == '\\') {
                    escaped = true;
                } else if (value == quote) {
                    break;
                }
            }
            tokens.push_back({TokenKind::String, source.substr(start, position - start), start});
            continue;
        }
        static const std::vector<std::string> multi = {
            "=>", "+=", "-=", "==", "!=", "<=", ">=", "&&", "||", "++", "--", "??", "?."
        };
        bool matched = false;
        for (const auto& symbol : multi) {
            if (source.compare(position, symbol.size(), symbol) == 0) {
                tokens.push_back({TokenKind::Symbol, symbol, position});
                position += symbol.size();
                matched = true;
                break;
            }
        }
        if (matched) continue;
        tokens.push_back({TokenKind::Symbol, std::string(1, current), position});
        ++position;
    }
    tokens.push_back({TokenKind::End, {}, source.size()});
    return tokens;
}

bool word(const Token& token, const char* value) {
    return token.kind == TokenKind::Identifier && token.text == value;
}

bool symbol(const Token& token, const char* value) {
    return token.kind == TokenKind::Symbol && token.text == value;
}

std::string sanitize(std::string value) {
    for (auto& character : value) {
        if (!isIdentifierPart(character)) character = '_';
    }
    if (value.empty() || !isIdentifierStart(value.front())) value.insert(value.begin(), '_');
    return value;
}

bool needsSpace(const Token& left, const Token& right) {
    if (left.kind == TokenKind::End || right.kind == TokenKind::End) return false;
    if ((left.kind == TokenKind::Identifier || left.kind == TokenKind::Number) &&
        (right.kind == TokenKind::Identifier || right.kind == TokenKind::Number)) return true;
    if (left.kind == TokenKind::String && right.kind == TokenKind::Identifier) return true;
    if (left.kind == TokenKind::Identifier && right.kind == TokenKind::String) return true;
    return false;
}

std::string emit(const std::vector<Token>& tokens, std::size_t begin = 0,
                 std::size_t end = std::numeric_limits<std::size_t>::max()) {
    end = std::min(end, tokens.size());
    std::ostringstream out;
    bool first = true;
    Token previous;
    for (std::size_t index = begin; index < end; ++index) {
        const auto& token = tokens[index];
        if (token.kind == TokenKind::End) break;
        if (!first && needsSpace(previous, token)) out << ' ';
        out << token.text;
        if (symbol(token, ";") || symbol(token, "{") || symbol(token, "}")) out << '\n';
        previous = token;
        first = false;
    }
    return out.str();
}

std::size_t matching(const std::vector<Token>& tokens, std::size_t open,
                     const char* left, const char* right) {
    int depth = 0;
    for (std::size_t index = open; index < tokens.size(); ++index) {
        if (symbol(tokens[index], left)) ++depth;
        else if (symbol(tokens[index], right) && --depth == 0) return index;
    }
    return tokens.size();
}

std::vector<std::vector<Token>> splitTopLevel(const std::vector<Token>& tokens,
                                               std::size_t begin,
                                               std::size_t end,
                                               const char* delimiter) {
    std::vector<std::vector<Token>> result;
    std::size_t start = begin;
    const bool trackAngles = std::string(delimiter) == ",";
    int parens = 0, brackets = 0, braces = 0, angles = 0;
    for (std::size_t index = begin; index < end; ++index) {
        if (symbol(tokens[index], "(")) ++parens;
        else if (symbol(tokens[index], ")")) --parens;
        else if (symbol(tokens[index], "[")) ++brackets;
        else if (symbol(tokens[index], "]")) --brackets;
        else if (symbol(tokens[index], "{")) ++braces;
        else if (symbol(tokens[index], "}")) --braces;
        else if (trackAngles && symbol(tokens[index], "<")) ++angles;
        else if (trackAngles && symbol(tokens[index], ">") && angles > 0) --angles;
        else if (symbol(tokens[index], delimiter) && parens == 0 && brackets == 0 && braces == 0 && angles == 0) {
            result.emplace_back(tokens.begin() + static_cast<std::ptrdiff_t>(start),
                                tokens.begin() + static_cast<std::ptrdiff_t>(index));
            start = index + 1;
        }
    }
    result.emplace_back(tokens.begin() + static_cast<std::ptrdiff_t>(start),
                        tokens.begin() + static_cast<std::ptrdiff_t>(end));
    return result;
}

std::string tokenText(const std::vector<Token>& tokens) {
    return emit(tokens);
}

struct InterfaceMethod {
    std::string name;
    std::size_t arity = 0;
};

struct InterfaceInfo {
    std::string name;
    std::vector<InterfaceMethod> methods;
};

struct DelegateInfo {
    std::string name;
    std::string returnType;
    std::vector<std::pair<std::string, std::string>> parameters;
};

struct GenericDecl {
    enum class Kind { Type, Function } kind = Kind::Type;
    std::string name;
    std::vector<std::string> parameters;
    std::vector<Token> tokens;
};

struct ReferenceParameterInfo {
    std::string modifier;
    std::string type;
    std::string name;
};

struct ReferenceFunctionInfo {
    std::string name;
    std::vector<ReferenceParameterInfo> parameters;
};

struct Context {
    std::string path;
    LanguageExpansionOptions options;
    LanguageExpansionResult result;
    std::map<std::string, InterfaceInfo> interfaces;
    std::map<std::string, DelegateInfo> delegates;
    std::map<std::string, GenericDecl> generics;
    std::map<std::pair<std::string, std::size_t>, ReferenceFunctionInfo>
        referenceFunctions;
    std::set<std::string> generatedRefTypes;
    std::uint64_t counter = 0;

    std::string unique(const char* prefix) {
        return std::string(prefix) + std::to_string(++counter);
    }

    void error(std::string code, std::string message, std::size_t offset = 0) {
        result.diagnostics.push_back({std::move(code), std::move(message), offset,
                                      LanguageExpansionSeverity::Error});
    }

    void warning(std::string code, std::string message, std::size_t offset = 0) {
        result.diagnostics.push_back({std::move(code), std::move(message), offset,
                                      LanguageExpansionSeverity::Warning});
    }
};

void rewriteValueAliases(std::vector<Token>& tokens, Context& context) {
    if (!context.options.valueTypeAliases) return;
    static const std::map<std::string, std::string> aliases = {
        {"byte", "int"}, {"sbyte", "int"}, {"short", "int"}, {"ushort", "int"},
        {"uint", "long"}, {"ulong", "long"}, {"float", "double"}, {"char", "int"}
    };
    for (auto& token : tokens) {
        if (token.kind != TokenKind::Identifier) continue;
        const auto found = aliases.find(token.text);
        if (found != aliases.end()) {
            token.text = found->second;
            context.result.changed = true;
        }
    }
}

std::vector<Token> removeRanges(const std::vector<Token>& tokens,
                                const std::vector<std::pair<std::size_t, std::size_t>>& ranges) {
    std::vector<Token> output;
    std::size_t rangeIndex = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        while (rangeIndex < ranges.size() && index >= ranges[rangeIndex].second) ++rangeIndex;
        if (rangeIndex < ranges.size() && index >= ranges[rangeIndex].first && index < ranges[rangeIndex].second) continue;
        output.push_back(tokens[index]);
    }
    return output;
}
