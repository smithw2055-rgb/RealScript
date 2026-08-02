        if (semicolon >= tokens.size()) break;
        std::size_t open = index;
        while (open < semicolon && !symbol(tokens[open], "(")) ++open;
        if (open < semicolon && open >= index + 3) {
            DelegateInfo info;
            info.returnType = tokens[index + 1].text;
            info.name = tokens[open - 1].text;
            const auto close = matching(tokens, open, "(", ")");
            if (close <= semicolon) {
                for (const auto& parameter : splitTopLevel(tokens, open + 1, close, ",")) {
                    if (parameter.size() >= 2) info.parameters.push_back({parameter[parameter.size() - 2].text, parameter.back().text});
                }
                context.delegates[info.name] = std::move(info);
            }
        }
        remove.push_back({index, semicolon + 1});
        index = semicolon + 1;
    }
    if (!remove.empty()) {
        tokens = removeRanges(tokens, remove);
        context.result.changed = true;
    }
}

std::vector<InterfaceMethod> parseInterfaceMethods(const std::vector<Token>& tokens,
                                                    std::size_t begin,
                                                    std::size_t end) {
    std::vector<InterfaceMethod> methods;
    std::size_t index = begin;
    while (index < end) {
        std::size_t semicolon = index;
        while (semicolon < end && !symbol(tokens[semicolon], ";")) ++semicolon;
        if (semicolon >= end) break;
        std::size_t open = index;
        while (open < semicolon && !symbol(tokens[open], "(")) ++open;
        if (open < semicolon && open > index) {
            const auto close = matching(tokens, open, "(", ")");
            if (close <= semicolon) {
                const auto parts = splitTopLevel(tokens, open + 1, close, ",");
                methods.push_back({tokens[open - 1].text,
                                   (parts.size() == 1 && parts.front().empty()) ? 0 : parts.size()});
            }
        }
        index = semicolon + 1;
    }
    return methods;
}

void extractInterfaces(std::vector<Token>& tokens, Context& context) {
    if (!context.options.interfaces) return;
    std::vector<std::pair<std::size_t, std::size_t>> remove;
    for (std::size_t index = 0; index + 2 < tokens.size();) {
        if (!word(tokens[index], "interface")) { ++index; continue; }
        if (tokens[index + 1].kind != TokenKind::Identifier || !symbol(tokens[index + 2], "{")) {
            context.error("RS8100", "invalid interface declaration", tokens[index].offset);
            ++index;
            continue;
        }
        const auto close = matching(tokens, index + 2, "{", "}");
        if (close >= tokens.size()) break;
        InterfaceInfo info;
        info.name = tokens[index + 1].text;
        info.methods = parseInterfaceMethods(tokens, index + 3, close);
        context.interfaces[info.name] = std::move(info);
        remove.push_back({index, close + 1});
        index = close + 1;
    }
    if (!remove.empty()) {
        tokens = removeRanges(tokens, remove);
        context.result.changed = true;
    }
}

std::set<std::pair<std::string, std::size_t>> classMethods(const std::vector<Token>& tokens,
                                                           std::size_t open,
                                                           std::size_t close) {
    std::set<std::pair<std::string, std::size_t>> result;
    int depth = 0;
    for (std::size_t index = open + 1; index < close; ++index) {
        if (symbol(tokens[index], "{")) { ++depth; continue; }
        if (symbol(tokens[index], "}")) { --depth; continue; }
        if (depth != 0 || !symbol(tokens[index], "(")) continue;
        if (index == 0 || tokens[index - 1].kind != TokenKind::Identifier) continue;
        const auto parenClose = matching(tokens, index, "(", ")");
        if (parenClose >= close) continue;
        const auto parts = splitTopLevel(tokens, index + 1, parenClose, ",");
        result.insert({tokens[index - 1].text,
                       (parts.size() == 1 && parts.front().empty()) ? 0 : parts.size()});
        index = parenClose;
    }
    return result;
}

void applyInterfaces(std::vector<Token>& tokens, Context& context) {
    if (!context.options.interfaces || context.interfaces.empty()) return;
    std::vector<Token> output;
    std::size_t index = 0;
    for (; index < tokens.size();) {
        if (!(word(tokens[index], "class") || word(tokens[index], "struct")) ||
            index + 2 >= tokens.size()) {
            output.push_back(tokens[index++]);
            continue;
        }
        const auto typeName = tokens[index + 1].text;
        output.push_back(tokens[index++]);
        output.push_back(tokens[index++]);
        std::vector<std::string> implemented;
        if (symbol(tokens[index], ":")) {
            ++index;
            while (index < tokens.size() && !symbol(tokens[index], "{")) {
                if (tokens[index].kind == TokenKind::Identifier) implemented.push_back(tokens[index].text);
                ++index;
            }
            context.result.changed = true;
        }
        if (index >= tokens.size() || !symbol(tokens[index], "{")) continue;
        const auto open = index;
        const auto close = matching(tokens, open, "{", "}");
        if (close >= tokens.size()) break;
        context.result.interfaces.push_back({typeName, implemented});
        const auto methods = classMethods(tokens, open, close);
        for (const auto& name : implemented) {
            const auto interfaceFound = context.interfaces.find(name);
            if (interfaceFound == context.interfaces.end()) {
                context.error("RS8101", "unknown interface '" + name + "'", tokens[open].offset);
                continue;
            }
            for (const auto& requirement : interfaceFound->second.methods) {
                if (methods.find({requirement.name, requirement.arity}) == methods.end()) {
                    context.error("RS8102", "type '" + typeName + "' does not implement '" +
                                  name + "." + requirement.name + "' with arity " +
                                  std::to_string(requirement.arity), tokens[open].offset);
                }
            }
        }
        for (std::size_t copy = open; copy <= close; ++copy) output.push_back(tokens[copy]);
        index = close + 1;
    }
    while (index < tokens.size()) output.push_back(tokens[index++]);
    tokens = std::move(output);
}

std::string typeKey(const std::vector<Token>& tokens) {
    std::string value;
    for (const auto& token : tokens) {
        if (token.kind == TokenKind::End) continue;
        value += token.text;
    }
    return value;
}

std::vector<std::string> parseTypeParameterNames(const std::vector<Token>& tokens,
                                                 std::size_t open,
                                                 std::size_t close) {
    std::vector<std::string> result;
    for (const auto& part : splitTopLevel(tokens, open + 1, close, ",")) {
        if (part.size() == 1 && part.front().kind == TokenKind::Identifier)
            result.push_back(part.front().text);
    }
    return result;
}

void collectGenericDeclarations(std::vector<Token>& tokens, Context& context) {
    if (!context.options.generics) return;
    std::vector<std::pair<std::size_t, std::size_t>> remove;
    int braceDepth = 0;
    for (std::size_t index = 0; index + 3 < tokens.size();) {
        if (symbol(tokens[index], "{")) { ++braceDepth; ++index; continue; }
        if (symbol(tokens[index], "}")) { --braceDepth; ++index; continue; }
        if ((word(tokens[index], "class") || word(tokens[index], "struct")) &&
            tokens[index + 1].kind == TokenKind::Identifier && symbol(tokens[index + 2], "<")) {
            const auto angleClose = matching(tokens, index + 2, "<", ">");
            if (angleClose >= tokens.size() || angleClose + 1 >= tokens.size() ||
                !symbol(tokens[angleClose + 1], "{")) { ++index; continue; }
            const auto bodyClose = matching(tokens, angleClose + 1, "{", "}");
            if (bodyClose >= tokens.size()) break;
            GenericDecl declaration;
            declaration.kind = GenericDecl::Kind::Type;
            declaration.name = tokens[index + 1].text;
            declaration.parameters = parseTypeParameterNames(tokens, index + 2, angleClose);
            declaration.tokens.assign(tokens.begin() + static_cast<std::ptrdiff_t>(index),
                                      tokens.begin() + static_cast<std::ptrdiff_t>(bodyClose + 1));
            context.generics[declaration.name] = std::move(declaration);
            remove.push_back({index, bodyClose + 1});
            index = bodyClose + 1;
            continue;
        }
        if (braceDepth == 0 && tokens[index].kind == TokenKind::Identifier &&
            index + 1 < tokens.size() && symbol(tokens[index + 1], "<")) {
            const auto angleClose = matching(tokens, index + 1, "<", ">");
            if (angleClose < tokens.size() && angleClose + 1 < tokens.size() &&
                symbol(tokens[angleClose + 1], "(")) {
                const auto parameterClose = matching(tokens, angleClose + 1, "(", ")");
                if (parameterClose < tokens.size() && parameterClose + 1 < tokens.size() &&
                    symbol(tokens[parameterClose + 1], "{")) {
                    std::size_t declarationStart = index;
                    while (declarationStart > 0 &&
                           !symbol(tokens[declarationStart - 1], ";") &&
                           !symbol(tokens[declarationStart - 1], "}")) --declarationStart;
                    const auto bodyClose = matching(tokens, parameterClose + 1, "{", "}");
                    GenericDecl declaration;
                    declaration.kind = GenericDecl::Kind::Function;
                    declaration.name = tokens[index].text;
                    declaration.parameters = parseTypeParameterNames(tokens, index + 1, angleClose);
                    declaration.tokens.assign(
                        tokens.begin() + static_cast<std::ptrdiff_t>(declarationStart),
                        tokens.begin() + static_cast<std::ptrdiff_t>(bodyClose + 1));
                    context.generics[declaration.name] = std::move(declaration);
                    remove.push_back({declarationStart, bodyClose + 1});
                    index = bodyClose + 1;
                    continue;
                }
            }
        }
        ++index;
    }
    if (!remove.empty()) {
        std::sort(remove.begin(), remove.end());
        tokens = removeRanges(tokens, remove);
        context.result.changed = true;
    }
}

std::string genericGeneratedName(const std::string& name,
                                 const std::vector<std::vector<Token>>& arguments) {
    std::string result = name;
    for (const auto& argument : arguments) result += "__" + sanitize(typeKey(argument));
    return result;
}

std::vector<Token> instantiateGeneric(const GenericDecl& declaration,
                                      const std::vector<std::vector<Token>>& arguments,
                                      const std::string& generatedName) {
    std::vector<Token> result;
    std::map<std::string, std::vector<Token>> substitutions;
    for (std::size_t index = 0; index < declaration.parameters.size(); ++index)
        substitutions[declaration.parameters[index]] = arguments[index];
    bool declarationNameSeen = false;
    for (std::size_t index = 0; index < declaration.tokens.size(); ++index) {
        const auto& token = declaration.tokens[index];
        if (token.kind == TokenKind::Identifier && token.text == declaration.name) {
            Token renamed = token;
            renamed.text = generatedName;
            result.push_back(std::move(renamed));
            if (index + 1 < declaration.tokens.size() && symbol(declaration.tokens[index + 1], "<")) {
                const auto close = matching(declaration.tokens, index + 1, "<", ">");
                index = close;
            }
            declarationNameSeen = true;
            continue;
        }
        const auto substitution = substitutions.find(token.text);
        if (token.kind == TokenKind::Identifier && substitution != substitutions.end()) {
            result.insert(result.end(), substitution->second.begin(), substitution->second.end());
        } else {
            result.push_back(token);
        }
    }
    (void)declarationNameSeen;
    return result;
}

std::size_t builtinGenericArity(const std::string& name) {
    if (name == "List" || name == "Queue" || name == "Stack" ||
        name == "Optional" || name == "HashSet") return 1;
    if (name == "Dictionary") return 2;
    return 0;
}

std::vector<Token> generateBuiltinGeneric(
    const std::string& name,
    const std::vector<std::vector<Token>>& arguments,
    const std::string& generatedName) {
    const auto first = tokenText(arguments.front());
    const auto second = arguments.size() > 1 ? tokenText(arguments[1]) : std::string{};
    std::ostringstream source;
    if (name == "List") {
        source << "class " << generatedName << "{" << first << "[] items;int count;"
               << generatedName << "(int capacity){items=new " << first << "[capacity];count=0;}"
               << "int Count(){return count;}"
               << "void Add(" << first << " value){items[count]=value;count=count+1;return;}"
               << first << " Get(int index){return items[index];}"
               << "void Set(int index," << first << " value){items[index]=value;return;}"
               << "void Clear(){count=0;return;}}";
    } else if (name == "Queue") {
        source << "class " << generatedName << "{" << first << "[] items;int head;int tail;int count;"
               << generatedName << "(int capacity){items=new " << first << "[capacity];head=0;tail=0;count=0;}"
               << "int Count(){return count;}"
               << "void Enqueue(" << first << " value){items[tail]=value;tail=(tail+1)%items.length;count=count+1;return;}"
               << first << " Dequeue(){" << first << " value=items[head];head=(head+1)%items.length;count=count-1;return value;}"
               << "void Clear(){head=0;tail=0;count=0;return;}}";
    } else if (name == "Stack") {
        source << "class " << generatedName << "{" << first << "[] items;int count;"
               << generatedName << "(int capacity){items=new " << first << "[capacity];count=0;}"
               << "int Count(){return count;}"
               << "void Push(" << first << " value){items[count]=value;count=count+1;return;}"
               << first << " Pop(){count=count-1;return items[count];}"
               << first << " Peek(){return items[count-1];}"
               << "void Clear(){count=0;return;}}";
    } else if (name == "Optional") {
        source << "class " << generatedName << "{bool hasValue;" << first << " value;"
               << generatedName << "(){hasValue=false;}"
               << generatedName << "(" << first << " initial){value=initial;hasValue=true;}"
               << "bool HasValue(){return hasValue;}"
               << first << " Value(){return value;}"
               << "void Set(" << first << " next){value=next;hasValue=true;return;}"
               << "void Clear(){hasValue=false;return;}}";
    } else if (name == "HashSet") {
        source << "class " << generatedName << "{" << first << "[] values;int count;"
               << generatedName << "(int capacity){values=new " << first << "[capacity];count=0;}"
               << "int Count(){return count;}"
               << "bool Contains(" << first << " value){int i=0;while(i<count){if(values[i]==value)return true;i=i+1;}return false;}"
               << "bool Add(" << first << " value){if(Contains(value))return false;values[count]=value;count=count+1;return true;}"
               << "void Clear(){count=0;return;}}";
    } else if (name == "Dictionary") {
        source << "class " << generatedName << "{" << first << "[] keys;" << second << "[] values;int count;"
               << generatedName << "(int capacity){keys=new " << first << "[capacity];values=new " << second << "[capacity];count=0;}"
               << "int Count(){return count;}"
               << "int Find(" << first << " key){int i=0;while(i<count){if(keys[i]==key)return i;i=i+1;}return -1;}"
               << "bool ContainsKey(" << first << " key){return Find(key)>=0;}"
               << "void Set(" << first << " key," << second << " value){int i=Find(key);if(i>=0){values[i]=value;return;}keys[count]=key;values[count]=value;count=count+1;return;}"
               << second << " Get(" << first << " key){return values[Find(key)];}"
               << "void Clear(){count=0;return;}}";
    }
    auto tokens = lex(source.str());
    if (!tokens.empty() && tokens.back().kind == TokenKind::End) tokens.pop_back();
    return tokens;
}

void instantiateGenerics(std::vector<Token>& tokens, Context& context) {
    if (!context.options.generics) return;
    std::vector<Token> generated;
    bool changed = true;
    std::set<std::string> instantiated;
    for (int pass = 0; pass < 32 && changed; ++pass) {
        changed = false;
        for (std::size_t index = 0; index + 2 < tokens.size(); ++index) {
            if (tokens[index].kind != TokenKind::Identifier || !symbol(tokens[index + 1], "<")) continue;
            const auto declaration = context.generics.find(tokens[index].text);
            const auto builtinArity = builtinGenericArity(tokens[index].text);
            if (declaration == context.generics.end() && builtinArity == 0) continue;
            const auto close = matching(tokens, index + 1, "<", ">");
            if (close >= tokens.size()) continue;
            const auto arguments = splitTopLevel(tokens, index + 2, close, ",");
            const auto expectedArity = declaration != context.generics.end()
                ? declaration->second.parameters.size() : builtinArity;
            if (arguments.size() != expectedArity) {
                context.error("RS8501", "generic arity mismatch for '" + tokens[index].text + "'", tokens[index].offset);
                continue;
            }
            const auto originalName = tokens[index].text;
            const auto name = genericGeneratedName(originalName, arguments);
            tokens[index].text = name;
            tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(index + 1),
                         tokens.begin() + static_cast<std::ptrdiff_t>(close + 1));
            context.result.genericInstantiations.push_back({originalName, {}, name});
            auto& record = context.result.genericInstantiations.back();
            for (const auto& argument : arguments) record.arguments.push_back(typeKey(argument));
            if (instantiated.insert(name).second) {
                const auto instance = declaration != context.generics.end()
                    ? instantiateGeneric(declaration->second, arguments, name)
                    : generateBuiltinGeneric(originalName, arguments, name);
                generated.insert(generated.end(), instance.begin(), instance.end());
                changed = true;
            }
            context.result.changed = true;
        }
        if (!generated.empty()) {
            const auto eof = tokens.empty() ? Token{} : tokens.back();
            if (!tokens.empty() && tokens.back().kind == TokenKind::End) tokens.pop_back();
            tokens.insert(tokens.end(), generated.begin(), generated.end());
            generated.clear();
            tokens.push_back(eof);
        }
    }
}
