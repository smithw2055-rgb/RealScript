        const auto arrayName = context_.unique("__rs_foreach_array_");
        const auto indexName = context_.unique("__rs_foreach_index_");
        const auto breakFlag = context_.unique("__rs_break_");
        const auto continueFlag = context_.unique("__rs_continue_");
        LoopContext loop{breakFlag, continueFlag, {}};
        const auto body = lowerStatement(closeParen + 1, end, &loop, {});

        std::string collectionVariable;
        std::string collectionType;
        if (in + 2 == closeParen && tokens_[in + 1].kind == TokenKind::Identifier) {
            collectionVariable = tokens_[in + 1].text;
            for (std::size_t scan = index; scan > 1; --scan) {
                const auto candidate = scan - 1;
                if (tokens_[candidate].kind != TokenKind::Identifier ||
                    tokens_[candidate].text != collectionVariable ||
                    candidate == 0 ||
                    tokens_[candidate - 1].kind != TokenKind::Identifier ||
                    candidate + 1 >= tokens_.size() ||
                    !(symbol(tokens_[candidate + 1], "=") ||
                      symbol(tokens_[candidate + 1], ";"))) {
                    continue;
                }
                collectionType = tokens_[candidate - 1].text;
                break;
            }
        }
        const bool indexedCollection =
            collectionType.rfind("List__", 0) == 0 ||
            collectionType.rfind("Queue__", 0) == 0 ||
            collectionType.rfind("Stack__", 0) == 0 ||
            collectionType.rfind("HashSet__", 0) == 0;

        std::ostringstream out;
        if (indexedCollection) {
            out << "{int " << indexName << "=0;bool " << breakFlag
                << "=false;while(" << indexName << "<" << collectionVariable
                << ".Count()&&!" << breakFlag << "){bool " << continueFlag
                << "=false;" << type << " " << name << "="
                << collectionVariable << ".Get(" << indexName << ");"
                << body.first << "if(!" << breakFlag << "){" << indexName
                << "=" << indexName << "+1;}}}\n";
        } else {
            out << "{" << type << "[] " << arrayName << "=" << collection
                << ";int " << indexName << "=0;bool " << breakFlag
                << "=false;while(" << indexName << "<" << arrayName
                << ".length&&!" << breakFlag << "){bool " << continueFlag
                << "=false;" << type << " " << name << "=" << arrayName
                << "[" << indexName << "];" << body.first << "if(!"
                << breakFlag << "){" << indexName << "=" << indexName
                << "+1;}}}\n";
        }
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

        struct CaseInfo {
            bool isDefault = false;
            std::string value;
            std::size_t begin = 0;
            std::size_t end = 0;
        };
        std::vector<CaseInfo> cases;
        std::size_t cursor = closeParen + 2;
        bool defaultSeen = false;
        while (cursor < closeBrace) {
            const bool isDefault = word(tokens_[cursor], "default");
            if (!isDefault && !word(tokens_[cursor], "case")) {
                ++cursor;
                continue;
            }
            if (isDefault && defaultSeen) {
                context_.error("RS8205", "switch contains more than one default label", tokens_[cursor].offset);
            }
            defaultSeen = defaultSeen || isDefault;
            std::size_t colon = cursor + 1;
            while (colon < closeBrace && !symbol(tokens_[colon], ":")) ++colon;
            if (colon >= closeBrace) {
                context_.error("RS8206", "switch label is missing ':'", tokens_[cursor].offset);
                break;
            }
            std::size_t nextCase = colon + 1;
            int depth = 0;
            while (nextCase < closeBrace) {
                if (symbol(tokens_[nextCase], "{")) ++depth;
                else if (symbol(tokens_[nextCase], "}")) --depth;
                if (depth == 0 &&
                    (word(tokens_[nextCase], "case") ||
                     word(tokens_[nextCase], "default"))) {
                    break;
                }
                ++nextCase;
            }
            cases.push_back(CaseInfo{
                isDefault,
                isDefault ? std::string{} : emit(tokens_, cursor + 1, colon),
                colon + 1,
                nextCase});
            cursor = nextCase;
        }

        std::ostringstream switchOutput;
        switchOutput << "{bool " << breakFlag << "=false;";
        bool emitted = false;
        for (const auto& item : cases) {
            if (item.isDefault) continue;
            switchOutput << (emitted ? "else if(" : "if(")
                << "(" << expression << ")== (" << item.value << "))";
            switchOutput << lowerBlockFromRange(item.begin, item.end, loop, breakFlag);
            emitted = true;
        }
        const auto defaultCase = std::find_if(
            cases.begin(), cases.end(),
            [](const CaseInfo& value) { return value.isDefault; });
        if (defaultCase != cases.end()) {
            switchOutput << (emitted ? "else" : "if(true)");
            switchOutput << lowerBlockFromRange(
                defaultCase->begin, defaultCase->end, loop, breakFlag);
        }
        switchOutput << "}\n";
        context_.result.changed = true;
        return {switchOutput.str(), closeBrace + 1};
    }

    std::string lowerBlockFromRange(std::size_t begin, std::size_t end,
                                    const LoopContext* loop,
                                    const std::string& switchBreak) {
        std::ostringstream out;
        out << "{\n";
