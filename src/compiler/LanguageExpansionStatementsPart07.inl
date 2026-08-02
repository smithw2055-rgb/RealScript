                }
            }
            rewritten.push_back(body[cursor++]);
        }

        output.push_back(tokens[open]);
        for (auto& [name, info] : events) {
            (void)name;
            for (const auto& handler : info.handlers) {
                auto field = lex("bool " + info.handlerFields[handler] + ";");
                if (!field.empty() && field.back().kind == TokenKind::End) field.pop_back();
                output.insert(output.end(), field.begin(), field.end());
            }
        }
        output.insert(output.end(), rewritten.begin(), rewritten.end());
        for (auto& [name, info] : events) {
            (void)name;
            output.insert(output.end(), info.generatedMembers.begin(), info.generatedMembers.end());
        }
        output.push_back(tokens[close]);
        index = close + 1;
    }
    while (index < tokens.size()) output.push_back(tokens[index++]);
    tokens = std::move(output);
}

void lowerSequences(std::vector<Token>& tokens, Context& context) {
    if (!context.options.deterministicCoroutines) return;
    std::vector<Token> output;
    std::size_t index = 0;
    for (; index < tokens.size();) {
        if (!word(tokens[index], "sequence")) {
            output.push_back(tokens[index++]);
            continue;
        }
        if (index + 3 >= tokens.size() || tokens[index + 1].kind != TokenKind::Identifier ||
            !symbol(tokens[index + 2], "(")) {
            context.error("RS8600", "invalid sequence declaration", tokens[index].offset);
            output.push_back(tokens[index++]);
            continue;
        }
        const auto name = tokens[index + 1].text;
        const auto closeParen = matching(tokens, index + 2, "(", ")");
        if (closeParen >= tokens.size() || closeParen + 1 >= tokens.size() ||
            !symbol(tokens[closeParen + 1], "{")) {
            context.error("RS8600", "invalid sequence declaration", tokens[index].offset);
            output.push_back(tokens[index++]);
            continue;
        }
        const auto closeBrace = matching(tokens, closeParen + 1, "{", "}");
        const auto parameters = splitTopLevel(tokens, index + 3, closeParen, ",");
        if (parameters.size() != 1 || parameters.front().size() < 2 ||
            parameters.front()[parameters.front().size() - 2].text != "long") {
            context.error("RS8601", "sequence must declare exactly one 'long target' parameter", tokens[index].offset);
            output.push_back(tokens[index++]);
            continue;
        }
        const auto targetName = parameters.front().back().text;
        std::vector<std::vector<Token>> segments;
        std::vector<std::vector<Token>> delays;
        std::size_t segmentStart = closeParen + 2;
        std::size_t cursor = segmentStart;
        int depth = 0;
        while (cursor < closeBrace) {
            if (symbol(tokens[cursor], "{")) ++depth;
            else if (symbol(tokens[cursor], "}")) --depth;
            if (depth == 0 && word(tokens[cursor], "yield") && cursor + 3 < closeBrace &&
                word(tokens[cursor + 1], "wait_ticks") && symbol(tokens[cursor + 2], "(")) {
                const auto waitClose = matching(tokens, cursor + 2, "(", ")");
                std::size_t semicolon = waitClose + 1;
                if (waitClose < closeBrace && semicolon < closeBrace && symbol(tokens[semicolon], ";")) {
                    segments.emplace_back(tokens.begin() + static_cast<std::ptrdiff_t>(segmentStart),
                                          tokens.begin() + static_cast<std::ptrdiff_t>(cursor));
                    delays.emplace_back(tokens.begin() + static_cast<std::ptrdiff_t>(cursor + 3),
                                        tokens.begin() + static_cast<std::ptrdiff_t>(waitClose));
                    segmentStart = semicolon + 1;
                    cursor = segmentStart;
                    continue;
                }
            }
