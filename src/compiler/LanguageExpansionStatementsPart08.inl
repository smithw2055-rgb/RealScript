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

void collectReferenceParameterDeclarations(
    const std::vector<Token>& tokens,
    Context& context) {
    if (!context.options.referenceParameters) return;

    for (std::size_t open = 1; open + 1 < tokens.size(); ++open) {
        if (!symbol(tokens[open], "(") ||
            tokens[open - 1].kind != TokenKind::Identifier ||
            isControlInvocationName(tokens[open - 1].text)) {
            continue;
        }
        const auto close = matching(tokens, open, "(", ")");
        if (close >= tokens.size() || close + 1 >= tokens.size() ||
            !symbol(tokens[close + 1], "{")) {
            continue;
        }

        ReferenceFunctionInfo info;
        info.name = tokens[open - 1].text;
        bool hasReferenceParameter = false;
        auto parameters = splitTopLevel(tokens, open + 1, close, ",");
        if (parameters.size() == 1 && parameters.front().empty()) {
            parameters.clear();
        }
        for (const auto& parameter : parameters) {
            ReferenceParameterInfo item;
            std::size_t cursor = 0;
            if (!parameter.empty() &&
                (word(parameter[0], "ref") || word(parameter[0], "out") ||
                 word(parameter[0], "in"))) {
                item.modifier = parameter[0].text;
                hasReferenceParameter = true;
                cursor = 1;
            }
            if (parameter.size() >= cursor + 2) {
                item.type = emit(parameter, cursor, parameter.size() - 1);
                item.name = parameter.back().text;
            }
            info.parameters.push_back(std::move(item));
        }
        if (!hasReferenceParameter) continue;

        const auto key = std::make_pair(info.name, info.parameters.size());
        const auto existing = context.referenceFunctions.find(key);
        if (existing == context.referenceFunctions.end()) {
            context.referenceFunctions.emplace(key, std::move(info));
            continue;
        }

        bool compatible = existing->second.parameters.size() == info.parameters.size();
        for (std::size_t parameter = 0;
             compatible && parameter < info.parameters.size(); ++parameter) {
            compatible =
                existing->second.parameters[parameter].modifier ==
                    info.parameters[parameter].modifier &&
                existing->second.parameters[parameter].type ==
                    info.parameters[parameter].type;
        }
        if (!compatible) {
            context.error(
                "RS8704",
                "reference-parameter overloads with the same name and arity are ambiguous for '" +
                    info.name + "'",
                tokens[open - 1].offset);
        }
    }
}

void lowerReferenceParameters(std::vector<Token>& tokens, Context& context) {
    if (!context.options.referenceParameters) return;

    using ParameterInfo = ReferenceParameterInfo;
    auto functions = context.referenceFunctions;

    // Kept as a scope boundary because the continuation lives in Part09.
    {
