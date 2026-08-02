            if (delegateFound == context.delegates.end()) {
                context.error("RS8301", "event uses unknown delegate type '" +
                              tokens[cursor + 1].text + "'", tokens[cursor].offset);
                continue;
            }
            EventInfo info;
            info.name = tokens[cursor + 2].text;
            info.signature = delegateFound->second;
            events.emplace(info.name, std::move(info));
            declarations.push_back({cursor, cursor + 4});
            cursor += 3;
        }

        if (events.empty()) {
            for (std::size_t copy = open; copy <= close; ++copy) output.push_back(tokens[copy]);
            index = close + 1;
            continue;
        }

        auto body = removeRanges(
            std::vector<Token>(tokens.begin() + static_cast<std::ptrdiff_t>(open + 1),
                               tokens.begin() + static_cast<std::ptrdiff_t>(close)),
            [&]() {
                std::vector<std::pair<std::size_t, std::size_t>> local;
                for (const auto& range : declarations)
                    local.push_back({range.first - (open + 1), range.second - (open + 1)});
                return local;
            }());
        if (!body.empty() && body.back().kind == TokenKind::End) body.pop_back();

        for (std::size_t cursor = 0; cursor + 2 < body.size(); ++cursor) {
            const auto eventFound = events.find(body[cursor].text);
            if (eventFound == events.end() ||
                !(symbol(body[cursor + 1], "+=") || symbol(body[cursor + 1], "-="))) continue;
            if (body[cursor + 2].kind == TokenKind::Identifier) {
                const auto handler = body[cursor + 2].text;
                auto& handlers = eventFound->second.handlers;
                if (std::find(handlers.begin(), handlers.end(), handler) == handlers.end())
                    handlers.push_back(handler);
                continue;
            }
            if (!symbol(body[cursor + 2], "(")) {
                context.error("RS8302", "event subscription requires a method group or lambda", body[cursor].offset);
                continue;
            }
            const auto lambdaClose = matching(body, cursor + 2, "(", ")");
            if (lambdaClose >= body.size() || lambdaClose + 1 >= body.size() ||
                !symbol(body[lambdaClose + 1], "=>")) {
                context.error("RS8302", "invalid event lambda", body[cursor].offset);
                continue;
            }
            auto& info = eventFound->second;
            const auto lambdaName = context.unique("__rs_lambda_");
            info.handlers.push_back(lambdaName);
            const auto lambdaParameters = splitTopLevel(body, cursor + 3, lambdaClose, ",");
            std::vector<std::string> names;
            for (std::size_t parameter = 0; parameter < info.signature.parameters.size(); ++parameter) {
                std::string name = info.signature.parameters[parameter].second;
                if (parameter < lambdaParameters.size() && !lambdaParameters[parameter].empty())
                    name = lambdaParameters[parameter].back().text;
                names.push_back(name);
            }
            std::ostringstream method;
            method << info.signature.returnType << ' ' << lambdaName << '(';
            for (std::size_t parameter = 0; parameter < info.signature.parameters.size(); ++parameter) {
                if (parameter != 0) method << ',';
                method << info.signature.parameters[parameter].first << ' ' << names[parameter];
            }
            method << ')';
            const auto lambdaBody = lambdaClose + 2;
            if (lambdaBody < body.size() && symbol(body[lambdaBody], "{")) {
                const auto lambdaBodyClose = matching(body, lambdaBody, "{", "}");
                method << emit(body, lambdaBody, lambdaBodyClose + 1);
            } else {
                std::size_t semicolon = lambdaBody;
                while (semicolon < body.size() && !symbol(body[semicolon], ";")) ++semicolon;
                method << '{';
                if (info.signature.returnType == "void") method << emit(body, lambdaBody, semicolon) << ";return;";
                else method << "return " << emit(body, lambdaBody, semicolon) << ';';
