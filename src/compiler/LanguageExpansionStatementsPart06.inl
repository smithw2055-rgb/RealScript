                method << '}';
            }
            auto generated = lex(method.str());
            if (!generated.empty() && generated.back().kind == TokenKind::End) generated.pop_back();
            info.generatedMembers.insert(info.generatedMembers.end(), generated.begin(), generated.end());
        }

        for (auto& [eventName, info] : events) {
            for (const auto& handler : info.handlers)
                info.handlerFields[handler] = "__rs_event_" + eventName + "_" + sanitize(handler);
        }

        std::vector<Token> rewritten;
        for (std::size_t cursor = 0; cursor < body.size();) {
            const auto eventFound = events.find(body[cursor].text);
            if (eventFound == events.end() || cursor + 1 >= body.size()) {
                rewritten.push_back(body[cursor++]);
                continue;
            }
            auto& info = eventFound->second;
            if (symbol(body[cursor + 1], "+=") || symbol(body[cursor + 1], "-=")) {
                const bool enable = symbol(body[cursor + 1], "+=");
                std::string handler;
                std::size_t endSite = cursor + 2;
                if (endSite < body.size() && body[endSite].kind == TokenKind::Identifier) {
                    handler = body[endSite].text;
                    ++endSite;
                } else if (endSite < body.size() && symbol(body[endSite], "(")) {
                    const auto lambdaClose = matching(body, endSite, "(", ")");
                    std::size_t lambdaBody = lambdaClose + 2;
                    if (lambdaBody < body.size() && symbol(body[lambdaBody], "{"))
                        endSite = matching(body, lambdaBody, "{", "}") + 1;
                    else {
                        endSite = lambdaBody;
                        while (endSite < body.size() && !symbol(body[endSite], ";")) ++endSite;
                    }
                    for (const auto& candidate : info.handlers) {
                        if (candidate.rfind("__rs_lambda_", 0) == 0 &&
                            std::none_of(rewritten.begin(), rewritten.end(), [&](const Token& token) {
                                return token.text.find(info.handlerFields[candidate]) != std::string::npos;
                            })) {
                            handler = candidate;
                            break;
                        }
                    }
                }
                while (endSite < body.size() && !symbol(body[endSite], ";")) ++endSite;
                if (endSite < body.size()) ++endSite;
                const auto field = info.handlerFields.find(handler);
                if (field == info.handlerFields.end()) {
                    context.error("RS8303", "event handler could not be resolved", body[cursor].offset);
                    cursor = endSite;
                    continue;
                }
                auto replacement = lex(field->second + std::string("=") + (enable ? "true;" : "false;"));
                if (!replacement.empty() && replacement.back().kind == TokenKind::End) replacement.pop_back();
                rewritten.insert(rewritten.end(), replacement.begin(), replacement.end());
                cursor = endSite;
                context.result.changed = true;
                continue;
            }
            if (symbol(body[cursor + 1], "(")) {
                const auto callClose = matching(body, cursor + 1, "(", ")");
                if (callClose < body.size() && callClose + 1 < body.size() && symbol(body[callClose + 1], ";")) {
                    const auto arguments = emit(body, cursor + 2, callClose);
                    std::ostringstream dispatch;
                    dispatch << '{';
                    for (const auto& handler : info.handlers) {
                        dispatch << "if(" << info.handlerFields[handler] << "){" << handler << '(' << arguments << ");}";
                    }
                    dispatch << '}';
                    auto replacement = lex(dispatch.str());
                    if (!replacement.empty() && replacement.back().kind == TokenKind::End) replacement.pop_back();
                    rewritten.insert(rewritten.end(), replacement.begin(), replacement.end());
                    cursor = callClose + 2;
                    context.result.changed = true;
                    continue;
