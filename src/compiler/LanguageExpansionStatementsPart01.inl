struct LoopContext {
    std::string breakFlag;
    std::string continueFlag;
    std::string increment;
};

class StatementLowerer {
public:
    StatementLowerer(const std::vector<Token>& tokens, Context& context)
        : tokens_(tokens), context_(context) {}

    std::string lowerBlock(std::size_t open, std::size_t close,
                           const LoopContext* loop = nullptr,
                           const std::string& switchBreak = {}) {
        std::ostringstream out;
        out << "{\n";
        std::size_t index = open + 1;
        while (index < close) {
            const auto lowered = lowerStatement(index, close, loop, switchBreak);
            if (lowered.second <= index) break;
            if (loop && !loop->breakFlag.empty()) {
                out << "if(!" << loop->breakFlag;
                if (!loop->continueFlag.empty()) out << "&&!" << loop->continueFlag;
                out << ")" << lowered.first;
            } else if (!switchBreak.empty()) {
                out << "if(!" << switchBreak << ")" << lowered.first;
            } else {
                out << lowered.first;
            }
            index = lowered.second;
        }
        out << "}\n";
        return out.str();
    }

private:
    std::pair<std::string, std::size_t> lowerStatement(
        std::size_t index, std::size_t end,
        const LoopContext* loop, const std::string& switchBreak) {
        if (index >= end) return {{}, end};
        if (symbol(tokens_[index], "{")) {
            const auto close = matching(tokens_, index, "{", "}");
            return {lowerBlock(index, close, loop, switchBreak), close + 1};
        }
        if (word(tokens_[index], "break")) {
            std::size_t next = index;
            while (next < end && !symbol(tokens_[next], ";")) ++next;
            const auto flag = !switchBreak.empty() ? switchBreak : (loop ? loop->breakFlag : std::string{});
            if (flag.empty()) context_.error("RS8201", "break is not inside a loop or switch", tokens_[index].offset);
            context_.result.changed = true;
            return {"{" + flag + "=true;}\n", std::min(next + 1, end)};
        }
        if (word(tokens_[index], "continue")) {
            std::size_t next = index;
            while (next < end && !symbol(tokens_[next], ";")) ++next;
            if (!loop || loop->continueFlag.empty()) context_.error("RS8202", "continue is not inside a loop", tokens_[index].offset);
            context_.result.changed = true;
            return {"{" + (loop ? loop->continueFlag : std::string{}) + "=true;}\n", std::min(next + 1, end)};
        }
        if (word(tokens_[index], "for")) return lowerFor(index, end, loop, switchBreak);
        if (word(tokens_[index], "foreach")) return lowerForeach(index, end, loop, switchBreak);
        if (word(tokens_[index], "do")) return lowerDo(index, end, loop, switchBreak);
        if (word(tokens_[index], "switch")) return lowerSwitch(index, end, loop);
        if (word(tokens_[index], "if") || word(tokens_[index], "while"))
            return lowerIfOrWhile(index, end, loop, switchBreak);
        return rawStatement(index, end);
    }

    std::pair<std::string, std::size_t> rawStatement(std::size_t index, std::size_t end) {
        std::size_t next = index;
        int parens = 0, brackets = 0;
        while (next < end) {
            if (symbol(tokens_[next], "(")) ++parens;
            else if (symbol(tokens_[next], ")")) --parens;
            else if (symbol(tokens_[next], "[")) ++brackets;
            else if (symbol(tokens_[next], "]")) --brackets;
            if (parens == 0 && brackets == 0 && symbol(tokens_[next], ";")) {
                ++next;
                break;
            }
