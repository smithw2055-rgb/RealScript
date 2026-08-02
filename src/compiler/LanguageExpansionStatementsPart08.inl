            ++cursor;
        }
        segments.emplace_back(tokens.begin() + static_cast<std::ptrdiff_t>(segmentStart),
                              tokens.begin() + static_cast<std::ptrdiff_t>(closeBrace));
        const auto fieldName = "__rs_sequence_target_" + name;
        std::ostringstream generated;
        generated << "long " << fieldName << ";void " << name << "(long " << targetName << "){" << fieldName
                  << "=" << targetName << ";" << tokenText(segments.front());
        if (!delays.empty()) generated << "Schedule(" << fieldName << ",\"__rs_sequence_" << name << "_1\"," << tokenText(delays.front()) << ");";
        generated << "}";
        for (std::size_t part = 1; part < segments.size(); ++part) {
            generated << "void __rs_sequence_" << name << "_" << part << "(){" << tokenText(segments[part]);
            if (part < delays.size()) generated << "Schedule(" << fieldName << ",\"__rs_sequence_" << name << "_" << (part + 1) << "\"," << tokenText(delays[part]) << ");";
            generated << "}";
        }
        auto replacement = lex(generated.str());
        if (!replacement.empty() && replacement.back().kind == TokenKind::End) replacement.pop_back();
        output.insert(output.end(), replacement.begin(), replacement.end());
        index = closeBrace + 1;
        context.result.changed = true;
    }
    while (index < tokens.size()) output.push_back(tokens[index++]);
    tokens = std::move(output);
}

bool isControlInvocationName(const std::string& name) {
    static const std::set<std::string> names = {
        "if", "while", "for", "foreach", "switch", "catch", "using", "lock"
    };
    return names.find(name) != names.end();
}

std::string referenceDefaultValue(const std::string& type) {
    if (type == "bool") return "false";
    if (type == "double") return "0.0";
    if (type == "string" || type.find("[]") != std::string::npos) return "null";
    if (type == "int" || type == "long") return "0";
    return "null";
}

void lowerReferenceParameters(std::vector<Token>& tokens, Context& context) {
    if (!context.options.referenceParameters) return;

    struct ParameterInfo {
        std::string modifier;
        std::string type;
        std::string name;
    };
    struct FunctionInfo {
        std::string name;
        std::vector<ParameterInfo> parameters;
    };

    std::map<std::pair<std::string, std::size_t>, FunctionInfo> functions;
    for (std::size_t open = 1; open + 1 < tokens.size(); ++open) {
        if (!symbol(tokens[open], "(") || tokens[open - 1].kind != TokenKind::Identifier ||
            isControlInvocationName(tokens[open - 1].text)) continue;
        const auto close = matching(tokens, open, "(", ")");
        if (close >= tokens.size() || close + 1 >= tokens.size() || !symbol(tokens[close + 1], "{")) continue;
        FunctionInfo info;
        info.name = tokens[open - 1].text;
        const auto parameters = splitTopLevel(tokens, open + 1, close, ",");
        for (const auto& parameter : parameters) {
            ParameterInfo item;
            std::size_t cursor = 0;
            if (!parameter.empty() && (word(parameter[0], "ref") || word(parameter[0], "out") || word(parameter[0], "in"))) {
                item.modifier = parameter[0].text;
                cursor = 1;
            }
            if (parameter.size() >= cursor + 2) {
                item.type = emit(parameter, cursor, parameter.size() - 1);
                item.name = parameter.back().text;
            }
            info.parameters.push_back(std::move(item));
        }
        const auto key = std::make_pair(info.name, info.parameters.size());
        if (functions.find(key) == functions.end()) functions.emplace(key, std::move(info));
