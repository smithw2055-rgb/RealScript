    }

    std::vector<Token> declarations;
    for (std::size_t index = 0; index < tokens.size();) {
        if (index + 1 < tokens.size() && tokens[index].kind == TokenKind::Identifier &&
            !isControlInvocationName(tokens[index].text) && symbol(tokens[index + 1], "(")) {
            const auto close = matching(tokens, index + 1, "(", ")");
            if (close < tokens.size() && close + 1 < tokens.size() && symbol(tokens[close + 1], "{")) {
                const auto key = std::make_pair(tokens[index].text,
                    [&]() {
                        const auto parts = splitTopLevel(tokens, index + 2, close, ",");
                        return (parts.size() == 1 && parts.front().empty()) ? std::size_t{0} : parts.size();
                    }());
                const auto function = functions.find(key);
                if (function != functions.end()) {
                    declarations.push_back(tokens[index++]);
                    declarations.push_back(tokens[index++]);
                    const auto parts = splitTopLevel(tokens, index, close, ",");
                    for (std::size_t parameter = 0; parameter < parts.size(); ++parameter) {
                        if (parameter != 0) declarations.push_back({TokenKind::Symbol, ",", 0});
                        const auto& info = function->second.parameters[parameter];
                        if (info.modifier == "ref" || info.modifier == "out") {
                            const auto wrapper = "__RsRef__" + sanitize(info.type);
                            context.generatedRefTypes.insert(wrapper + "|" + info.type);
                            auto replacement = lex(wrapper + " " + info.name);
                            if (!replacement.empty() && replacement.back().kind == TokenKind::End) replacement.pop_back();
                            declarations.insert(declarations.end(), replacement.begin(), replacement.end());
                        } else if (info.modifier == "in") {
                            auto replacement = lex(info.type + " " + info.name);
                            if (!replacement.empty() && replacement.back().kind == TokenKind::End) replacement.pop_back();
                            declarations.insert(declarations.end(), replacement.begin(), replacement.end());
                        } else {
                            declarations.insert(declarations.end(), parts[parameter].begin(), parts[parameter].end());
                        }
                    }
                    declarations.push_back(tokens[close]);
                    const auto bodyOpen = close + 1;
                    const auto bodyClose = matching(tokens, bodyOpen, "{", "}");
                    declarations.push_back(tokens[bodyOpen]);
                    for (std::size_t body = bodyOpen + 1; body < bodyClose; ++body) {
                        bool rewritten = false;
                        for (const auto& parameter : function->second.parameters) {
                            if (parameter.modifier != "ref" && parameter.modifier != "out" && parameter.modifier != "in") continue;
                            if (tokens[body].kind != TokenKind::Identifier || tokens[body].text != parameter.name) continue;
                            if (parameter.modifier == "in" && body + 1 < bodyClose && symbol(tokens[body + 1], "=")) {
                                context.error("RS8702", "cannot assign to in parameter '" + parameter.name + "'", tokens[body].offset);
                            }
                            if (parameter.modifier == "ref" || parameter.modifier == "out") {
                                declarations.push_back(tokens[body]);
                                declarations.push_back({TokenKind::Symbol, ".", tokens[body].offset});
                                declarations.push_back({TokenKind::Identifier, "Value", tokens[body].offset});
                                rewritten = true;
                            }
                            break;
                        }
                        if (!rewritten) declarations.push_back(tokens[body]);
                    }
                    declarations.push_back(tokens[bodyClose]);
                    index = bodyClose + 1;
                    context.result.changed = true;
                    continue;
                }
            }
        }
        declarations.push_back(tokens[index++]);
    }
    tokens = std::move(declarations);

    struct Replacement {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::vector<Token> tokens;
    };
    std::vector<Replacement> replacements;

    for (std::size_t open = 1; open + 1 < tokens.size(); ++open) {
        if (!symbol(tokens[open], "(") || tokens[open - 1].kind != TokenKind::Identifier) continue;
        const auto close = matching(tokens, open, "(", ")");
