        std::size_t index = begin;
        while (index < end) {
            const auto statement = lowerStatement(index, end, loop, switchBreak);
            if (statement.second <= index) break;
            out << "if(!" << switchBreak;
            if (loop && !loop->breakFlag.empty()) {
                out << "&&!" << loop->breakFlag;
            }
            if (loop && !loop->continueFlag.empty()) {
                out << "&&!" << loop->continueFlag;
            }
            out << ")" << statement.first;
            index = statement.second;
        }
        out << "}\n";
        return out.str();
    }

    const std::vector<Token>& tokens_;
    Context& context_;
};

void lowerFunctionBodies(std::vector<Token>& tokens, Context& context) {
    if (!context.options.structuredControlFlow) return;
    std::vector<Token> output;
    std::size_t index = 0;
    for (; index < tokens.size();) {
        if (symbol(tokens[index], "{") && index > 0 &&
            (symbol(tokens[index - 1], ")") || word(tokens[index - 1], "get") || word(tokens[index - 1], "set"))) {
            const auto close = matching(tokens, index, "{", "}");
            if (close >= tokens.size()) break;
            StatementLowerer lowerer(tokens, context);
            const auto text = lowerer.lowerBlock(index, close);
            auto replacement = lex(text);
            if (!replacement.empty() && replacement.back().kind == TokenKind::End) replacement.pop_back();
            output.insert(output.end(), replacement.begin(), replacement.end());
            index = close + 1;
            continue;
        }
        output.push_back(tokens[index++]);
    }
    while (index < tokens.size()) output.push_back(tokens[index++]);
    tokens = std::move(output);
}

// Event lowering is event-scoped. Delegate declarations define the event
// signature, subscriptions are represented by deterministic boolean slots, and
// field/this-capturing lambdas become generated instance methods. Local-variable
// captures intentionally remain unsupported until closure objects are added.
void lowerEvents(std::vector<Token>& tokens, Context& context) {
    if (!context.options.delegatesLambdasEvents || context.delegates.empty()) return;

    struct EventInfo {
        std::string name;
        DelegateInfo signature;
        std::vector<std::string> handlers;
        std::map<std::string, std::string> handlerFields;
        std::vector<Token> generatedMembers;
    };

    std::vector<Token> output;
    std::size_t index = 0;
    while (index < tokens.size()) {
        if (!word(tokens[index], "class") || index + 2 >= tokens.size()) {
            output.push_back(tokens[index++]);
            continue;
        }
        output.push_back(tokens[index++]);
        output.push_back(tokens[index++]);
        if (!symbol(tokens[index], "{")) continue;
        const auto open = index;
        const auto close = matching(tokens, open, "{", "}");
        if (close >= tokens.size()) break;

        std::map<std::string, EventInfo> events;
        std::vector<std::pair<std::size_t, std::size_t>> declarations;
        int depth = 0;
        for (std::size_t cursor = open + 1; cursor < close; ++cursor) {
            if (symbol(tokens[cursor], "{")) { ++depth; continue; }
            if (symbol(tokens[cursor], "}")) { --depth; continue; }
            if (depth != 0 || !word(tokens[cursor], "event")) continue;
            if (cursor + 3 >= close || tokens[cursor + 1].kind != TokenKind::Identifier ||
                tokens[cursor + 2].kind != TokenKind::Identifier || !symbol(tokens[cursor + 3], ";")) {
                context.error("RS8300", "invalid event declaration", tokens[cursor].offset);
                continue;
            }
            const auto delegateFound = context.delegates.find(tokens[cursor + 1].text);
