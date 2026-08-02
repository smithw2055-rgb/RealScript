        if (close >= tokens.size() || close + 1 >= tokens.size() || !symbol(tokens[close + 1], ";")) continue;
        const auto arguments = splitTopLevel(tokens, open + 1, close, ",");
        const auto arity = (arguments.size() == 1 && arguments.front().empty()) ? std::size_t{0} : arguments.size();
        const auto function = functions.find({tokens[open - 1].text, arity});
        if (function == functions.end()) continue;
        const auto hasReferences = std::any_of(function->second.parameters.begin(), function->second.parameters.end(),
            [](const ParameterInfo& parameter) { return !parameter.modifier.empty(); });
        if (!hasReferences) continue;

        std::size_t callStart = open - 1;
        while (callStart >= 2 && symbol(tokens[callStart - 1], ".") &&
               tokens[callStart - 2].kind == TokenKind::Identifier) callStart -= 2;
        const auto callHead = emit(tokens, callStart, open);
        std::ostringstream lowered;
        lowered << '{';
        std::vector<std::string> callArguments;
        std::vector<std::pair<std::string, std::string>> writebacks;
        for (std::size_t parameter = 0; parameter < arity; ++parameter) {
            const auto& info = function->second.parameters[parameter];
            auto argument = arguments[parameter];
            if (info.modifier.empty()) {
                callArguments.push_back(tokenText(argument));
                continue;
            }
            if (argument.empty() || !word(argument.front(), info.modifier.c_str())) {
                context.error("RS8703", "argument " + std::to_string(parameter) + " for '" +
                              function->second.name + "' must use " + info.modifier, tokens[open].offset);
                callArguments.push_back(tokenText(argument));
                continue;
            }
            argument.erase(argument.begin());
            const auto lvalue = tokenText(argument);
            if (info.modifier == "in") {
                callArguments.push_back(lvalue);
                continue;
            }
            const auto wrapper = referenceWrapperName(function->second, info.type);
            if (function->second.moduleName == context.moduleName) {
                context.generatedRefTypes.insert(wrapper + "|" + info.type);
            }
            const auto temporary = context.unique(info.modifier == "out" ? "__rs_out_" : "__rs_ref_");
            lowered << wrapper << ' ' << temporary << "=new " << wrapper << '(';
            lowered << (info.modifier == "out" ? referenceDefaultValue(info.type) : lvalue) << ");";
            callArguments.push_back(temporary);
            writebacks.push_back({lvalue, temporary});
        }
        lowered << callHead << '(';
        for (std::size_t argument = 0; argument < callArguments.size(); ++argument) {
            if (argument != 0) lowered << ',';
            lowered << callArguments[argument];
        }
        lowered << ");";
        for (const auto& writeback : writebacks)
            lowered << writeback.first << '=' << writeback.second << ".Value;";
        lowered << '}';
        auto replacement = lex(lowered.str());
        if (!replacement.empty() && replacement.back().kind == TokenKind::End) replacement.pop_back();
        replacements.push_back({callStart, close + 2, std::move(replacement)});
    }

    if (!replacements.empty()) {
        std::sort(replacements.begin(), replacements.end(),
            [](const Replacement& left, const Replacement& right) { return left.begin < right.begin; });
        std::vector<Token> output;
        std::size_t cursor = 0;
        for (const auto& replacement : replacements) {
            if (replacement.begin < cursor) continue;
            output.insert(output.end(), tokens.begin() + static_cast<std::ptrdiff_t>(cursor),
                          tokens.begin() + static_cast<std::ptrdiff_t>(replacement.begin));
            output.insert(output.end(), replacement.tokens.begin(), replacement.tokens.end());
            cursor = replacement.end;
        }
        output.insert(output.end(), tokens.begin() + static_cast<std::ptrdiff_t>(cursor), tokens.end());
        tokens = std::move(output);
        context.result.changed = true;
    }
}

void appendGeneratedSupport(std::vector<Token>& tokens, Context& context) {
    std::ostringstream generated;
    for (const auto& encoded : context.generatedRefTypes) {
        const auto separator = encoded.find('|');
