        const auto arrayName = context_.unique("__rs_foreach_array_");
        const auto indexName = context_.unique("__rs_foreach_index_");
        const auto breakFlag = context_.unique("__rs_break_");
        const auto continueFlag = context_.unique("__rs_continue_");
        LoopContext loop{breakFlag, continueFlag, {}};
        const auto body = lowerStatement(closeParen + 1, end, &loop, {});
        std::ostringstream out;
        out << "{" << type << "[] " << arrayName << "=" << collection << ";int " << indexName
            << "=0;bool " << breakFlag << "=false;while(" << indexName << "<" << arrayName
            << ".length&&!" << breakFlag << "){bool " << continueFlag << "=false;" << type << " "
            << name << "=" << arrayName << "[" << indexName << "];" << body.first << "if(!" << breakFlag
            << "){" << indexName << "=" << indexName << "+1;}}}\n";
        context_.result.changed = true;
        return {out.str(), body.second};
    }

    std::pair<std::string, std::size_t> lowerDo(
        std::size_t index, std::size_t end,
        const LoopContext*, const std::string&) {
        const auto breakFlag = context_.unique("__rs_break_");
        const auto continueFlag = context_.unique("__rs_continue_");
        LoopContext loop{breakFlag, continueFlag, {}};
        const auto body = lowerStatement(index + 1, end, &loop, {});
        std::size_t next = body.second;
        if (next >= end || !word(tokens_[next], "while") || next + 1 >= end ||
            !symbol(tokens_[next + 1], "(")) return rawStatement(index, end);
        const auto closeParen = matching(tokens_, next + 1, "(", ")");
        const auto condition = emit(tokens_, next + 2, closeParen);
        next = closeParen + 1;
        if (next < end && symbol(tokens_[next], ";")) ++next;
        const auto first = context_.unique("__rs_first_");
        std::ostringstream out;
        out << "{bool " << first << "=true;bool " << breakFlag << "=false;while((" << first << "||(" << condition
            << "))&&!" << breakFlag << "){" << first << "=false;bool " << continueFlag << "=false;" << body.first << "}}\n";
        context_.result.changed = true;
        return {out.str(), next};
    }

    std::pair<std::string, std::size_t> lowerSwitch(
        std::size_t index, std::size_t end, const LoopContext* loop) {
        if (index + 1 >= end || !symbol(tokens_[index + 1], "(")) return rawStatement(index, end);
        const auto closeParen = matching(tokens_, index + 1, "(", ")");
        if (closeParen + 1 >= end || !symbol(tokens_[closeParen + 1], "{")) return rawStatement(index, end);
        const auto closeBrace = matching(tokens_, closeParen + 1, "{", "}");
        const auto expression = emit(tokens_, index + 2, closeParen);
        const auto breakFlag = context_.unique("__rs_switch_break_");
        std::ostringstream out;
        out << "{bool " << breakFlag << "=false;";
        std::size_t cursor = closeParen + 2;
        bool firstCase = true;
        while (cursor < closeBrace) {
            bool isDefault = word(tokens_[cursor], "default");
            if (!isDefault && !word(tokens_[cursor], "case")) { ++cursor; continue; }
            std::size_t colon = cursor + 1;
            while (colon < closeBrace && !symbol(tokens_[colon], ":")) ++colon;
            std::size_t nextCase = colon + 1;
            int depth = 0;
            while (nextCase < closeBrace) {
                if (symbol(tokens_[nextCase], "{")) ++depth;
                else if (symbol(tokens_[nextCase], "}")) --depth;
                if (depth == 0 && (word(tokens_[nextCase], "case") || word(tokens_[nextCase], "default"))) break;
                ++nextCase;
            }
            const auto caseValue = isDefault ? std::string{} : emit(tokens_, cursor + 1, colon);
            out << (firstCase ? "if(" : "else if(")
                << (isDefault ? "true" : "(" + expression + ")== (" + caseValue + ")") << ")";
            out << lowerBlockFromRange(colon + 1, nextCase, loop, breakFlag);
            firstCase = false;
            cursor = nextCase;
        }
        out << "}\n";
        context_.result.changed = true;
        return {out.str(), closeBrace + 1};
    }

    std::string lowerBlockFromRange(std::size_t begin, std::size_t end,
                                    const LoopContext* loop,
                                    const std::string& switchBreak) {
        std::ostringstream out;
        out << "{\n";
