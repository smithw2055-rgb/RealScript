            if (parens == 0 && brackets == 0 && symbol(tokens_[next], "{")) {
                const auto close = matching(tokens_, next, "{", "}");
                next = close + 1;
                break;
            }
            ++next;
        }
        return {emit(tokens_, index, next), next};
    }

    std::pair<std::string, std::size_t> lowerIfOrWhile(
        std::size_t index, std::size_t end,
        const LoopContext* outerLoop, const std::string& switchBreak) {
        const bool isWhile = word(tokens_[index], "while");
        if (index + 1 >= end || !symbol(tokens_[index + 1], "(")) return rawStatement(index, end);
        const auto closeParen = matching(tokens_, index + 1, "(", ")");
        if (closeParen >= end) return rawStatement(index, end);
        const auto condition = emit(tokens_, index + 2, closeParen);
        std::size_t bodyStart = closeParen + 1;
        std::pair<std::string, std::size_t> body;
        if (isWhile) {
            LoopContext loop{context_.unique("__rs_break_"), context_.unique("__rs_continue_"), {}};
            body = lowerStatement(bodyStart, end, &loop, {});
            std::ostringstream out;
            out << "{bool " << loop.breakFlag << "=false;while((" << condition << ")&&!" << loop.breakFlag
                << "){bool " << loop.continueFlag << "=false;" << body.first << "}}\n";
            context_.result.changed = true;
            return {out.str(), body.second};
        }
        body = lowerStatement(bodyStart, end, outerLoop, switchBreak);
        std::ostringstream out;
        out << "if(" << condition << ")" << body.first;
        std::size_t next = body.second;
        if (next < end && word(tokens_[next], "else")) {
            const auto otherwise = lowerStatement(next + 1, end, outerLoop, switchBreak);
            out << "else" << otherwise.first;
            next = otherwise.second;
        }
        return {out.str(), next};
    }

    std::pair<std::string, std::size_t> lowerFor(
        std::size_t index, std::size_t end,
        const LoopContext*, const std::string&) {
        if (index + 1 >= end || !symbol(tokens_[index + 1], "(")) return rawStatement(index, end);
        const auto closeParen = matching(tokens_, index + 1, "(", ")");
        auto clauses = splitTopLevel(tokens_, index + 2, closeParen, ";");
        if (clauses.size() != 3) {
            context_.error("RS8203", "for statement requires initializer, condition, and increment", tokens_[index].offset);
            return rawStatement(index, end);
        }
        const auto breakFlag = context_.unique("__rs_break_");
        const auto continueFlag = context_.unique("__rs_continue_");
        LoopContext loop{breakFlag, continueFlag, tokenText(clauses[2])};
        const auto body = lowerStatement(closeParen + 1, end, &loop, {});
        const auto condition = clauses[1].empty() ? "true" : tokenText(clauses[1]);
        std::ostringstream out;
        out << "{" << tokenText(clauses[0]);
        if (!clauses[0].empty()) out << ";";
        out << "bool " << breakFlag << "=false;while((" << condition << ")&&!" << breakFlag << ")"
            << "{bool " << continueFlag << "=false;" << body.first
            << "if(!" << breakFlag << "){" << tokenText(clauses[2]) << ";}}}\n";
        context_.result.changed = true;
        return {out.str(), body.second};
    }

    std::pair<std::string, std::size_t> lowerForeach(
        std::size_t index, std::size_t end,
        const LoopContext*, const std::string&) {
        if (index + 1 >= end || !symbol(tokens_[index + 1], "(")) return rawStatement(index, end);
        const auto closeParen = matching(tokens_, index + 1, "(", ")");
        std::size_t in = index + 2;
        while (in < closeParen && !word(tokens_[in], "in")) ++in;
        if (in >= closeParen || in < index + 4) {
            context_.error("RS8204", "foreach requires 'type name in array'", tokens_[index].offset);
            return rawStatement(index, end);
        }
        const auto type = emit(tokens_, index + 2, in - 1);
        const auto name = tokens_[in - 1].text;
        const auto collection = emit(tokens_, in + 1, closeParen);
