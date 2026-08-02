        const auto name = encoded.substr(0, separator);
        const auto type = encoded.substr(separator + 1);
        generated << "class " << name << "{" << type << " Value;" << name << "(" << type
                  << " value){Value=value;}}";
    }
    if (!context.result.succeeded()) {
        std::size_t index = 0;
        for (const auto& diagnostic : context.result.diagnostics) {
            if (diagnostic.severity != LanguageExpansionSeverity::Error) continue;
            generated << "void __rs_expansion_error_" << index++ << "(){__rs_error_"
                      << sanitize(diagnostic.code) << "();}";
        }
    }
    if (generated.str().empty()) return;
    if (!tokens.empty() && tokens.back().kind == TokenKind::End) tokens.pop_back();
    auto support = lex(generated.str());
    if (!support.empty() && support.back().kind == TokenKind::End) support.pop_back();
    tokens.insert(tokens.end(), support.begin(), support.end());
    tokens.push_back({TokenKind::End, {}, 0});
}
