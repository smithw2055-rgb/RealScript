void extractAttributes(std::vector<Token>& tokens, Context& context) {
    if (!context.options.sourceAttributes) return;
    std::vector<std::pair<std::size_t, std::size_t>> remove;
    std::size_t index = 0;
    while (index + 1 < tokens.size()) {
        if (!symbol(tokens[index], "[")) { ++index; continue; }
        const bool declarationPosition = index == 0 ||
            symbol(tokens[index - 1], ";") || symbol(tokens[index - 1], "{") ||
            symbol(tokens[index - 1], "}") || symbol(tokens[index - 1], "]");
        if (!declarationPosition || index + 1 >= tokens.size() ||
            tokens[index + 1].kind != TokenKind::Identifier) {
            ++index;
            continue;
        }
        const auto close = matching(tokens, index, "[", "]");
        if (close >= tokens.size()) break;
        std::size_t targetIndex = close + 1;
        while (targetIndex < tokens.size() &&
               (word(tokens[targetIndex], "public") || word(tokens[targetIndex], "private") ||
                word(tokens[targetIndex], "internal") || word(tokens[targetIndex], "protected") ||
                word(tokens[targetIndex], "static") || word(tokens[targetIndex], "readonly"))) ++targetIndex;
        if (targetIndex >= tokens.size()) break;
        std::string target = tokens[targetIndex].text;
        if ((word(tokens[targetIndex], "class") || word(tokens[targetIndex], "struct") ||
             word(tokens[targetIndex], "enum") || word(tokens[targetIndex], "interface")) &&
            targetIndex + 1 < tokens.size()) target = tokens[targetIndex + 1].text;
        else if (targetIndex + 1 < tokens.size() && tokens[targetIndex + 1].kind == TokenKind::Identifier)
            target = tokens[targetIndex + 1].text;
        if (!context.moduleName.empty()) {
            target = context.moduleName + "::" + target;
        }

        auto attributes = splitTopLevel(tokens, index + 1, close, ",");
        for (const auto& attributeTokens : attributes) {
            if (attributeTokens.empty()) continue;
            LanguageAttributeRecord record;
            record.target = target;
            record.name = attributeTokens.front().text;
            std::size_t open = attributeTokens.size();
            for (std::size_t i = 1; i < attributeTokens.size(); ++i) {
                if (symbol(attributeTokens[i], "(")) { open = i; break; }
            }
            if (open < attributeTokens.size()) {
                const auto closeLocal = matching(attributeTokens, open, "(", ")");
                if (closeLocal < attributeTokens.size()) {
                    auto args = splitTopLevel(attributeTokens, open + 1, closeLocal, ",");
                    std::size_t positional = 0;
                    for (const auto& arg : args) {
                        if (arg.empty()) continue;
                        std::size_t equals = arg.size();
                        for (std::size_t j = 0; j < arg.size(); ++j) {
                            if (symbol(arg[j], "=")) { equals = j; break; }
                        }
                        LanguageAttributeArgument value;
                        if (equals < arg.size()) {
                            value.name = tokenText(std::vector<Token>(arg.begin(), arg.begin() + static_cast<std::ptrdiff_t>(equals)));
                            value.value = tokenText(std::vector<Token>(arg.begin() + static_cast<std::ptrdiff_t>(equals + 1), arg.end()));
                        } else {
                            value.name = "arg" + std::to_string(positional++);
                            value.value = tokenText(arg);
                        }
                        record.arguments.push_back(std::move(value));
                    }
                }
            }
            context.result.attributes.push_back(std::move(record));
        }
        remove.push_back({index, close + 1});
        index = close + 1;
    }
    if (!remove.empty()) {
        tokens = removeRanges(tokens, remove);
        context.result.changed = true;
    }
}

void extractDelegates(std::vector<Token>& tokens, Context& context) {
    if (!context.options.delegatesLambdasEvents) return;
    std::vector<std::pair<std::size_t, std::size_t>> remove;
    for (std::size_t index = 0; index < tokens.size();) {
        if (!word(tokens[index], "delegate")) { ++index; continue; }
        std::size_t semicolon = index;
        while (semicolon < tokens.size() && !symbol(tokens[semicolon], ";")) ++semicolon;
