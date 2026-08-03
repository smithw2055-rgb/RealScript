// Native bounded generic specialization for Phase 18.
// Included inside compiler::anonymous namespace after ParsedUnit/ModuleWork.

struct NativeGenericTemplate {
    enum class Kind { Class, Struct, Function };
    Kind kind = Kind::Class;
    std::string moduleName;
    std::string name;
    std::string sourceName;
    text::TextSpan span;
    std::vector<std::string> parameters;
    const ParsedUnit* unit = nullptr;
};

struct NativeGenericRegistry {
    std::map<std::string, std::map<std::string, NativeGenericTemplate>> types;
    std::map<std::string, std::map<std::string, NativeGenericTemplate>> functions;
};

std::string nativeGenericSanitize(std::string value) {
    for (auto& character : value) {
        const auto alphaNumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '_';
        if (!alphaNumeric) character = '_';
    }
    return value;
}

std::string nativeGenericGeneratedName(
    const std::string& name,
    const std::vector<std::string>& arguments) {
    auto result = name;
    for (const auto& argument : arguments) {
        result += "__" + nativeGenericSanitize(argument);
    }
    return result;
}

std::size_t nativeBuiltinGenericArity(const std::string& name) {
    if (name == "List" || name == "Queue" || name == "Stack" ||
        name == "Optional" || name == "HashSet") {
        return 1;
    }
    if (name == "Dictionary") return 2;
    return 0;
}

std::string nativeBuiltinGenericSource(
    const std::string& name,
    const std::vector<std::string>& arguments,
    const std::string& generatedName) {
    if (arguments.empty()) return {};
    const auto& first = arguments.front();
    const auto second = arguments.size() > 1
        ? arguments[1]
        : std::string{};
    std::ostringstream source;
    if (name == "List") {
        source << "class " << generatedName << "{" << first
            << "[] items;int count;"
            << generatedName
            << "(int capacity){items=new " << first
            << "[capacity];count=0;}"
            << "int Count(){return count;}"
            << "void Add(" << first
            << " value){items[count]=value;count=count+1;return;}"
            << first << " Get(int index){return items[index];}"
            << "void Set(int index," << first
            << " value){items[index]=value;return;}"
            << "void Clear(){count=0;return;}}";
    } else if (name == "Queue") {
        source << "class " << generatedName << "{" << first
            << "[] items;int head;int tail;int count;"
            << generatedName
            << "(int capacity){items=new " << first
            << "[capacity];head=0;tail=0;count=0;}"
            << "int Count(){return count;}"
            << "void Enqueue(" << first
            << " value){items[tail]=value;tail=(tail+1)%items.length;"
            << "count=count+1;return;}"
            << first << " Dequeue(){" << first
            << " value=items[head];head=(head+1)%items.length;"
            << "count=count-1;return value;}"
            << first
            << " Get(int index){return items[(head+index)%items.length];}"
            << "void Clear(){head=0;tail=0;count=0;return;}}";
    } else if (name == "Stack") {
        source << "class " << generatedName << "{" << first
            << "[] items;int count;"
            << generatedName
            << "(int capacity){items=new " << first
            << "[capacity];count=0;}"
            << "int Count(){return count;}"
            << "void Push(" << first
            << " value){items[count]=value;count=count+1;return;}"
            << first
            << " Pop(){count=count-1;return items[count];}"
            << first << " Peek(){return items[count-1];}"
            << first << " Get(int index){return items[index];}"
            << "void Clear(){count=0;return;}}";
    } else if (name == "Optional") {
        source << "class " << generatedName
            << "{bool hasValue;" << first << " value;"
            << generatedName << "(){hasValue=false;}"
            << generatedName << "(" << first
            << " initial){value=initial;hasValue=true;}"
            << "bool HasValue(){return hasValue;}"
            << first << " Value(){return value;}"
            << "void Set(" << first
            << " next){value=next;hasValue=true;return;}"
            << "void Clear(){hasValue=false;return;}}";
    } else if (name == "HashSet") {
        source << "class " << generatedName << "{" << first
            << "[] values;int count;"
            << generatedName
            << "(int capacity){values=new " << first
            << "[capacity];count=0;}"
            << "int Count(){return count;}"
            << "bool Contains(" << first
            << " value){int i=0;while(i<count){"
            << "if(values[i]==value)return true;i=i+1;}return false;}"
            << "bool Add(" << first
            << " value){if(Contains(value))return false;"
            << "values[count]=value;count=count+1;return true;}"
            << first << " Get(int index){return values[index];}"
            << "void Clear(){count=0;return;}}";
    } else if (name == "Dictionary") {
        if (arguments.size() != 2) return {};
        source << "class " << generatedName << "{" << first
            << "[] keys;" << second << "[] values;int count;"
            << generatedName
            << "(int capacity){keys=new " << first
            << "[capacity];values=new " << second
            << "[capacity];count=0;}"
            << "int Count(){return count;}"
            << "int Find(" << first
            << " key){int i=0;while(i<count){if(keys[i]==key)return i;"
            << "i=i+1;}return -1;}"
            << "bool ContainsKey(" << first
            << " key){return Find(key)>=0;}"
            << "void Set(" << first << " key," << second
            << " value){int i=Find(key);if(i>=0){values[i]=value;return;}"
            << "keys[count]=key;values[count]=value;count=count+1;return;}"
            << second << " Get(" << first
            << " key){return values[Find(key)];}"
            << "void Clear(){count=0;return;}}";
    }
    return source.str();
}

std::size_t nativeMatchingAngle(
    const std::vector<syntax::SyntaxToken>& tokens,
    std::size_t open) {
    int depth = 0;
    for (std::size_t index = open; index < tokens.size(); ++index) {
        if (tokens[index].kind == syntax::SyntaxKind::LessToken) ++depth;
        else if (tokens[index].kind == syntax::SyntaxKind::GreaterToken) {
            --depth;
            if (depth == 0) return index;
        }
    }
    return tokens.size();
}

std::string nativeSpecializedDeclarationSource(
    const NativeGenericTemplate& declaration,
    const std::vector<std::string>& arguments,
    const std::string& generatedName,
    diagnostics::DiagnosticBag& diagnostics) {
    if (!declaration.unit || !declaration.unit->source ||
        declaration.parameters.size() != arguments.size() ||
        declaration.span.end() > declaration.unit->source->content().size()) {
        return {};
    }
    const auto fragment = declaration.unit->source->content().substr(
        declaration.span.start, declaration.span.length);
    text::SourceText source(fragment, declaration.sourceName);
    diagnostics::DiagnosticBag lexerDiagnostics;
    syntax::Lexer lexer(source, lexerDiagnostics);
    auto tokens = lexer.lexAll();
    diagnostics.append(lexerDiagnostics);

    std::map<std::string, std::string> substitutions;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        substitutions.emplace(declaration.parameters[index], arguments[index]);
    }

    std::ostringstream output;
    bool declarationParametersRemoved = false;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if (token.kind == syntax::SyntaxKind::EndOfFileToken) continue;
        if (token.kind == syntax::SyntaxKind::IdentifierToken &&
            token.text == declaration.name) {
            output << generatedName << ' ';
            if (index + 1 < tokens.size() &&
                tokens[index + 1].kind == syntax::SyntaxKind::LessToken) {
                const auto close = nativeMatchingAngle(tokens, index + 1);
                if (close < tokens.size()) {
                    index = close;
                    declarationParametersRemoved = true;
                }
            }
            continue;
        }
        const auto substitution = substitutions.find(token.text);
        if (token.kind == syntax::SyntaxKind::IdentifierToken &&
            substitution != substitutions.end()) {
            output << substitution->second << ' ';
        } else {
            output << token.text << ' ';
        }
    }
    if (!declarationParametersRemoved) {
        diagnostics.report(
            "RS8510",
            "generic declaration could not be specialized",
            declaration.span,
            diagnostics::DiagnosticSeverity::Error,
            declaration.sourceName);
    }
    return output.str();
}

class NativeGenericSpecializer {
public:
    NativeGenericSpecializer(
        std::vector<std::unique_ptr<ParsedUnit>>& units,
        std::map<std::string, ModuleWork>& modules,
        BuildResult& result)
        : units_(units), modules_(modules), result_(result) {}

    void run() {
        collectTemplates();
        std::vector<ParsedUnit*> queue;
        for (auto& unit : units_) queue.push_back(unit.get());
        for (std::size_t index = 0; index < queue.size(); ++index) {
            auto* unit = queue[index];
            if (!unit || !processed_.insert(unit).second) continue;
            rewriteUnit(*unit, queue);
        }
        std::stable_sort(
            result_.nativeGenericInstantiations.begin(),
            result_.nativeGenericInstantiations.end(),
            [](const auto& left, const auto& right) {
                if (left.generatedName != right.generatedName) {
                    return left.generatedName < right.generatedName;
                }
                if (left.genericName != right.genericName) {
                    return left.genericName < right.genericName;
                }
                return left.arguments < right.arguments;
            });
        result_.nativeGenericInstantiations.erase(
            std::unique(
                result_.nativeGenericInstantiations.begin(),
                result_.nativeGenericInstantiations.end(),
                [](const auto& left, const auto& right) {
                    return left.generatedName == right.generatedName &&
                        left.genericName == right.genericName &&
                        left.arguments == right.arguments;
                }),
            result_.nativeGenericInstantiations.end());
    }

private:
    void collectTemplates() {
        for (auto& [moduleName, module] : modules_) {
            for (const auto* unit : module.units) {
                for (const auto& declaration : unit->syntaxTree->classes) {
                    if (declaration.typeParameters.empty()) continue;
                    addTypeTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Class);
                }
                for (const auto& declaration : unit->syntaxTree->structs) {
                    if (declaration.typeParameters.empty()) continue;
                    addTypeTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Struct);
                }
                for (const auto& declaration : unit->syntaxTree->functions) {
                    if (declaration.typeParameters.empty()) continue;
                    addFunctionTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.span(),
                        unit);
                }
                for (const auto& declaration : unit->syntaxTree->classes) {
                    for (const auto& method : declaration.methods) {
                        if (!method.typeParameters.empty()) {
                            result_.diagnostics.report(
                                "RS8521",
                                "generic member methods are reserved for Phase 21",
                                method.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                unit->source->name());
                            module.invalid = true;
                        }
                    }
                }
                for (const auto& declaration : unit->syntaxTree->structs) {
                    for (const auto& method : declaration.methods) {
                        if (!method.typeParameters.empty()) {
                            result_.diagnostics.report(
                                "RS8521",
                                "generic member methods are reserved for Phase 21",
                                method.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                unit->source->name());
                            module.invalid = true;
                        }
                    }
                }
            }
        }
    }

    void addTypeTemplate(
        const std::string& moduleName,
        const std::string& name,
        const std::vector<syntax::SyntaxToken>& parameters,
        text::TextSpan span,
        const ParsedUnit* unit,
        NativeGenericTemplate::Kind kind) {
        NativeGenericTemplate declaration;
        declaration.kind = kind;
        declaration.moduleName = moduleName;
        declaration.name = name;
        declaration.sourceName = unit->source->name();
        declaration.span = span;
        declaration.unit = unit;
        for (const auto& parameter : parameters) {
            declaration.parameters.push_back(parameter.text);
        }
        auto& table = registry_.types[moduleName];
        if (!table.emplace(name, std::move(declaration)).second) {
            result_.diagnostics.report(
                "RS8502",
                "duplicate generic type template '" + name + "'",
                span,
                diagnostics::DiagnosticSeverity::Error,
                unit->source->name());
            modules_[moduleName].invalid = true;
        }
    }

    void addFunctionTemplate(
        const std::string& moduleName,
        const std::string& name,
        const std::vector<syntax::SyntaxToken>& parameters,
        text::TextSpan span,
        const ParsedUnit* unit) {
        NativeGenericTemplate declaration;
        declaration.kind = NativeGenericTemplate::Kind::Function;
        declaration.moduleName = moduleName;
        declaration.name = name;
        declaration.sourceName = unit->source->name();
        declaration.span = span;
        declaration.unit = unit;
        for (const auto& parameter : parameters) {
            declaration.parameters.push_back(parameter.text);
        }
        auto& table = registry_.functions[moduleName];
        if (!table.emplace(name, std::move(declaration)).second) {
            result_.diagnostics.report(
                "RS8502",
                "duplicate generic function template '" + name + "'",
                span,
                diagnostics::DiagnosticSeverity::Error,
                unit->source->name());
            modules_[moduleName].invalid = true;
        }
    }

    const NativeGenericTemplate* resolve(
        const std::string& moduleName,
        const std::string& name,
        bool function,
        text::TextSpan span,
        const std::string& sourceName) {
        const auto& tables = function ? registry_.functions : registry_.types;
        const auto own = tables.find(moduleName);
        if (own != tables.end()) {
            const auto found = own->second.find(name);
            if (found != own->second.end()) return &found->second;
        }
        std::vector<const NativeGenericTemplate*> candidates;
        const auto module = modules_.find(moduleName);
        if (module != modules_.end()) {
            for (const auto& importedName : module->second.imports) {
                const auto imported = tables.find(importedName);
                if (imported == tables.end()) continue;
                const auto found = imported->second.find(name);
                if (found != imported->second.end()) {
                    candidates.push_back(&found->second);
                }
            }
        }
        if (candidates.size() == 1) return candidates.front();
        if (candidates.size() > 1) {
            result_.diagnostics.report(
                "RS8503",
                "generic declaration '" + name + "' is ambiguous",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[moduleName].invalid = true;
        }
        return nullptr;
    }

    std::string typeText(const syntax::TypeSyntax& type) const {
        auto result = type.name.text;
        if (type.isGeneric()) {
            result.push_back('<');
            for (std::size_t index = 0;
                 index < type.typeArguments.size(); ++index) {
                if (index != 0) result.push_back(',');
                result += typeText(type.typeArguments[index]);
            }
            result.push_back('>');
        }
        if (type.isArray()) result += "[]";
        return result;
    }

    std::string request(
        const std::string& consumerModule,
        const std::string& name,
        std::vector<std::string> arguments,
        bool function,
        text::TextSpan span,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        const auto builtinArity = function
            ? std::size_t{0}
            : nativeBuiltinGenericArity(name);
        const auto* declaration = resolve(
            consumerModule, name, function, span, sourceName);
        if (!declaration && builtinArity == 0) {
            result_.diagnostics.report(
                "RS8500",
                "unknown generic declaration '" + name + "'",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return name;
        }
        const auto expectedArity = declaration
            ? declaration->parameters.size()
            : builtinArity;
        if (arguments.size() != expectedArity) {
            result_.diagnostics.report(
                "RS8501",
                "generic arity mismatch for '" + name + "'",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return name;
        }

        const auto targetModule = declaration
            ? declaration->moduleName
            : consumerModule;
        const auto generatedName = nativeGenericGeneratedName(name, arguments);
        const auto key = targetModule + "::" +
            (function ? "function:" : "type:") + generatedName;
        if (!specializations_.insert(key).second) return generatedName;

        std::string body;
        if (declaration) {
            body = nativeSpecializedDeclarationSource(
                *declaration,
                arguments,
                generatedName,
                result_.diagnostics);
        } else {
            body = nativeBuiltinGenericSource(name, arguments, generatedName);
        }
        if (body.empty()) {
            result_.diagnostics.report(
                "RS8510",
                "generic specialization '" + generatedName +
                    "' could not be generated",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return generatedName;
        }

        std::ostringstream generated;
        generated << "module " << targetModule << ";\n" << body << '\n';
        const auto path = "$generated/generics/" +
            nativeGenericSanitize(targetModule) + "/" +
            generatedName + ".rs";
        auto unit = std::make_unique<ParsedUnit>();
        unit->source = std::make_unique<text::SourceText>(
            generated.str(), path);
        diagnostics::DiagnosticBag parseDiagnostics;
        syntax::Parser parser(*unit->source, parseDiagnostics);
        unit->syntaxTree = std::make_unique<syntax::CompilationUnitSyntax>(
            parser.parseCompilationUnit());
        unit->moduleName = targetModule;
        unit->sourceFingerprint = stableFingerprint(
            path + "\n" + generated.str());
        unit->invalid = parseDiagnostics.hasErrors();
        for (const auto& diagnostic : parseDiagnostics.items()) {
            result_.diagnostics.report(
                diagnostic.code,
                diagnostic.message,
                diagnostic.span,
                diagnostic.severity,
                path);
        }
        auto* pointer = unit.get();
        auto& target = modules_[targetModule];
        target.name = targetModule;
        target.units.push_back(pointer);
        target.invalid = target.invalid || unit->invalid;
        units_.push_back(std::move(unit));
        queue.push_back(pointer);
        result_.nativeGenericInstantiations.push_back(
            LanguageGenericInstantiation{name, arguments, generatedName});
        return generatedName;
    }

    void clearGenericType(syntax::TypeSyntax& type, std::string name) {
        type.name.text = std::move(name);
        type.lessToken.reset();
        type.typeArguments.clear();
        type.typeArgumentCommaTokens.clear();
        type.greaterToken.reset();
    }

    void rewriteType(
        syntax::TypeSyntax& type,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        if (!type.isGeneric()) return;
        for (auto& argument : type.typeArguments) {
            rewriteType(argument, moduleName, sourceName, queue);
        }
        std::vector<std::string> arguments;
        for (const auto& argument : type.typeArguments) {
            arguments.push_back(typeText(argument));
        }
        const auto generated = request(
            moduleName,
            type.name.text,
            std::move(arguments),
            false,
            type.span(),
            sourceName,
            queue);
        clearGenericType(type, generated);
    }

    void rewriteExpression(
        syntax::ExpressionSyntax& expression,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        switch (expression.kind()) {
        case syntax::SyntaxKind::LambdaExpression:
            rewriteExpression(
                *static_cast<syntax::LambdaExpressionSyntax&>(expression).body,
                moduleName, sourceName, queue);
            return;
        case syntax::SyntaxKind::UnaryExpression:
            rewriteExpression(
                *static_cast<syntax::UnaryExpressionSyntax&>(expression).operand,
                moduleName, sourceName, queue);
            return;
        case syntax::SyntaxKind::BinaryExpression: {
            auto& value = static_cast<syntax::BinaryExpressionSyntax&>(expression);
            rewriteExpression(*value.left, moduleName, sourceName, queue);
            rewriteExpression(*value.right, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::AssignmentExpression:
            rewriteExpression(
                *static_cast<syntax::AssignmentExpressionSyntax&>(expression).expression,
                moduleName, sourceName, queue);
            return;
        case syntax::SyntaxKind::ParenthesizedExpression:
            rewriteExpression(
                *static_cast<syntax::ParenthesizedExpressionSyntax&>(expression).expression,
                moduleName, sourceName, queue);
            return;
        case syntax::SyntaxKind::CallExpression: {
            auto& call = static_cast<syntax::CallExpressionSyntax&>(expression);
            if (call.lessToken) {
                for (auto& argument : call.typeArguments) {
                    rewriteType(argument, moduleName, sourceName, queue);
                }
                std::vector<std::string> arguments;
                for (const auto& argument : call.typeArguments) {
                    arguments.push_back(typeText(argument));
                }
                call.identifierToken.text = request(
                    moduleName,
                    call.identifierToken.text,
                    std::move(arguments),
                    true,
                    call.span(),
                    sourceName,
                    queue);
                call.lessToken.reset();
                call.typeArguments.clear();
                call.typeArgumentCommaTokens.clear();
                call.greaterToken.reset();
            }
            for (auto& argument : call.arguments) {
                rewriteExpression(*argument, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::MemberCallExpression: {
            auto& call = static_cast<syntax::MemberCallExpressionSyntax&>(expression);
            rewriteExpression(*call.receiver, moduleName, sourceName, queue);
            if (call.lessToken) {
                result_.diagnostics.report(
                    "RS8521",
                    "generic member methods are reserved for Phase 21",
                    call.span(),
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[moduleName].invalid = true;
            }
            for (auto& argument : call.arguments) {
                rewriteExpression(*argument, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::MemberAccessExpression:
            rewriteExpression(
                *static_cast<syntax::MemberAccessExpressionSyntax&>(expression).receiver,
                moduleName, sourceName, queue);
            return;
        case syntax::SyntaxKind::ElementAccessExpression: {
            auto& value = static_cast<syntax::ElementAccessExpressionSyntax&>(expression);
            rewriteExpression(*value.receiver, moduleName, sourceName, queue);
            rewriteExpression(*value.index, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::ElementAssignmentExpression: {
            auto& value = static_cast<syntax::ElementAssignmentExpressionSyntax&>(expression);
            rewriteExpression(*value.receiver, moduleName, sourceName, queue);
            rewriteExpression(*value.index, moduleName, sourceName, queue);
            rewriteExpression(*value.expression, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::MemberAssignmentExpression: {
            auto& value = static_cast<syntax::MemberAssignmentExpressionSyntax&>(expression);
            rewriteExpression(*value.receiver, moduleName, sourceName, queue);
            rewriteExpression(*value.expression, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::NewObjectExpression: {
            auto& value = static_cast<syntax::NewObjectExpressionSyntax&>(expression);
            rewriteType(value.type, moduleName, sourceName, queue);
            for (auto& argument : value.arguments) {
                rewriteExpression(*argument, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::NewArrayExpression: {
            auto& value = static_cast<syntax::NewArrayExpressionSyntax&>(expression);
            rewriteType(value.elementType, moduleName, sourceName, queue);
            rewriteExpression(*value.length, moduleName, sourceName, queue);
            return;
        }
        default:
            return;
        }
    }

    void rewriteStatement(
        syntax::StatementSyntax& statement,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        switch (statement.kind()) {
        case syntax::SyntaxKind::BlockStatement:
            for (auto& child : static_cast<syntax::BlockStatementSyntax&>(statement).statements) {
                rewriteStatement(*child, moduleName, sourceName, queue);
            }
            return;
        case syntax::SyntaxKind::ReturnStatement: {
            auto& value = static_cast<syntax::ReturnStatementSyntax&>(statement);
            if (value.expression) {
                rewriteExpression(*value.expression, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::IfStatement: {
            auto& value = static_cast<syntax::IfStatementSyntax&>(statement);
            rewriteExpression(*value.condition, moduleName, sourceName, queue);
            rewriteStatement(*value.thenStatement, moduleName, sourceName, queue);
            if (value.elseStatement) {
                rewriteStatement(*value.elseStatement, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::WhileStatement: {
            auto& value = static_cast<syntax::WhileStatementSyntax&>(statement);
            rewriteExpression(*value.condition, moduleName, sourceName, queue);
            rewriteStatement(*value.body, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::ForStatement: {
            auto& value = static_cast<syntax::ForStatementSyntax&>(statement);
            if (value.initializer) rewriteStatement(*value.initializer, moduleName, sourceName, queue);
            if (value.condition) rewriteExpression(*value.condition, moduleName, sourceName, queue);
            if (value.increment) rewriteExpression(*value.increment, moduleName, sourceName, queue);
            rewriteStatement(*value.body, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::ForeachStatement: {
            auto& value = static_cast<syntax::ForeachStatementSyntax&>(statement);
            rewriteType(value.type, moduleName, sourceName, queue);
            rewriteExpression(*value.collection, moduleName, sourceName, queue);
            rewriteStatement(*value.body, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::DoWhileStatement: {
            auto& value = static_cast<syntax::DoWhileStatementSyntax&>(statement);
            rewriteStatement(*value.body, moduleName, sourceName, queue);
            rewriteExpression(*value.condition, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::SwitchStatement: {
            auto& value = static_cast<syntax::SwitchStatementSyntax&>(statement);
            rewriteExpression(*value.expression, moduleName, sourceName, queue);
            for (auto& section : value.sections) {
                if (section.label) rewriteExpression(*section.label, moduleName, sourceName, queue);
                for (auto& child : section.statements) rewriteStatement(*child, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::YieldWaitStatement:
            rewriteExpression(
                *static_cast<syntax::YieldWaitStatementSyntax&>(statement).delay,
                moduleName, sourceName, queue);
            return;
        case syntax::SyntaxKind::EventSubscriptionStatement:
            rewriteExpression(
                *static_cast<syntax::EventSubscriptionStatementSyntax&>(statement).handler,
                moduleName, sourceName, queue);
            return;
        case syntax::SyntaxKind::VariableDeclarationStatement: {
            auto& value = static_cast<syntax::VariableDeclarationStatementSyntax&>(statement);
            rewriteType(value.type, moduleName, sourceName, queue);
            if (value.initializer) rewriteExpression(*value.initializer, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::ExpressionStatement:
            rewriteExpression(
                *static_cast<syntax::ExpressionStatementSyntax&>(statement).expression,
                moduleName, sourceName, queue);
            return;
        default:
            return;
        }
    }

    void rewriteParameter(
        syntax::ParameterSyntax& parameter,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        rewriteType(parameter.type, moduleName, sourceName, queue);
    }

    void rewriteFunction(
        syntax::FunctionDeclarationSyntax& function,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        if (!function.typeParameters.empty()) return;
        rewriteType(function.returnType, moduleName, sourceName, queue);
        for (auto& parameter : function.parameters) rewriteParameter(parameter, moduleName, sourceName, queue);
        rewriteStatement(function.body, moduleName, sourceName, queue);
    }

    template <typename TypeDeclaration>
    void rewriteTypeDeclaration(
        TypeDeclaration& declaration,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        if (!declaration.typeParameters.empty()) return;
        for (auto& interfaceType : declaration.interfaces) rewriteType(interfaceType, moduleName, sourceName, queue);
        for (auto& field : declaration.fields) rewriteType(field.type, moduleName, sourceName, queue);
        for (auto& event : declaration.events) rewriteType(event.delegateType, moduleName, sourceName, queue);
        for (auto& method : declaration.methods) rewriteFunction(method, moduleName, sourceName, queue);
        for (auto& sequence : declaration.sequences) {
            for (auto& parameter : sequence.parameters) rewriteParameter(parameter, moduleName, sourceName, queue);
            rewriteStatement(sequence.body, moduleName, sourceName, queue);
        }
        for (auto& constructor : declaration.constructors) {
            for (auto& parameter : constructor.parameters) rewriteParameter(parameter, moduleName, sourceName, queue);
            rewriteStatement(constructor.body, moduleName, sourceName, queue);
        }
        for (auto& property : declaration.properties) {
            rewriteType(property.type, moduleName, sourceName, queue);
            if (property.getter && property.getter->body) rewriteStatement(*property.getter->body, moduleName, sourceName, queue);
            if (property.setter && property.setter->body) rewriteStatement(*property.setter->body, moduleName, sourceName, queue);
        }
    }

    void rewriteUnit(ParsedUnit& unit, std::vector<ParsedUnit*>& queue) {
        const auto& moduleName = unit.moduleName;
        const auto sourceName = unit.source->name();
        for (auto& declaration : unit.syntaxTree->classes) rewriteTypeDeclaration(declaration, moduleName, sourceName, queue);
        for (auto& declaration : unit.syntaxTree->structs) rewriteTypeDeclaration(declaration, moduleName, sourceName, queue);
        for (auto& declaration : unit.syntaxTree->interfaces) {
            for (auto& method : declaration.methods) {
                rewriteType(method.returnType, moduleName, sourceName, queue);
                for (auto& parameter : method.parameters) rewriteParameter(parameter, moduleName, sourceName, queue);
            }
        }
        for (auto& declaration : unit.syntaxTree->delegates) {
            rewriteType(declaration.returnType, moduleName, sourceName, queue);
            for (auto& parameter : declaration.parameters) rewriteParameter(parameter, moduleName, sourceName, queue);
        }
        for (auto& function : unit.syntaxTree->functions) rewriteFunction(function, moduleName, sourceName, queue);
    }

    std::vector<std::unique_ptr<ParsedUnit>>& units_;
    std::map<std::string, ModuleWork>& modules_;
    BuildResult& result_;
    NativeGenericRegistry registry_;
    std::set<std::string> specializations_;
    std::set<ParsedUnit*> processed_;
};

void specializeNativeGenerics(
    std::vector<std::unique_ptr<ParsedUnit>>& units,
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    NativeGenericSpecializer specializer(units, modules, result);
    specializer.run();
}
