// Native bounded generic specialization for Phase 18.
// Included inside compiler::anonymous namespace after ParsedUnit/ModuleWork.

struct NativeGenericTemplate {
    enum class Kind { Class, Struct, Interface, Delegate, Function };
    Kind kind = Kind::Class;
    std::string moduleName;
    std::string ownerName;
    std::string name;
    std::string sourceName;
    text::TextSpan span;
    std::vector<std::string> parameters;
    std::vector<std::string> parameterTypes;
    std::map<std::string, std::vector<std::string>> constraints;
    const ParsedUnit* unit = nullptr;
};

struct NativeGenericRegistry {
    std::map<std::string, std::map<std::string, NativeGenericTemplate>> types;
    std::map<std::string, std::map<std::string, NativeGenericTemplate>> functions;
    std::map<std::string,
        std::map<std::string, std::vector<NativeGenericTemplate>>> members;
};

struct PendingNativeGenericMember {
    const NativeGenericTemplate* declaration = nullptr;
    std::vector<std::string> arguments;
    std::string generatedName;
    text::TextSpan requestSpan;
    std::string sourceName;
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

std::string nativeGenericTypeText(const syntax::TypeSyntax& type) {
    auto result = type.name.text;
    if (type.isGeneric()) {
        result.push_back('<');
        for (std::size_t index = 0; index < type.typeArguments.size(); ++index) {
            if (index != 0) result.push_back(',');
            result += nativeGenericTypeText(type.typeArguments[index]);
        }
        result.push_back('>');
    }
    if (type.isArray()) result += "[]";
    return result;
}

std::map<std::string, std::vector<std::string>> nativeGenericConstraints(
    const std::vector<syntax::GenericConstraintClauseSyntax>& clauses) {
    std::map<std::string, std::vector<std::string>> result;
    for (const auto& clause : clauses) {
        std::string current;
        int angleDepth = 0;
        for (const auto& token : clause.constraints) {
            if (token.kind == syntax::SyntaxKind::LessToken) ++angleDepth;
            if (token.kind == syntax::SyntaxKind::GreaterToken) --angleDepth;
            if (token.kind == syntax::SyntaxKind::CommaToken && angleDepth == 0) {
                if (!current.empty()) result[clause.typeParameter.text].push_back(current);
                current.clear();
            } else {
                current += token.text;
            }
        }
        if (!current.empty()) result[clause.typeParameter.text].push_back(current);
    }
    return result;
}

std::size_t nativeBuiltinGenericArity(const std::string& name) {
    if (name == "List" || name == "Queue" || name == "Stack" ||
        name == "Optional" || name == "Nullable" || name == "Box" ||
        name == "HashSet") {
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
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] items;int count;"
            << generatedName << "(){items=new " << first << "[4];count=0;}"
            << generatedName << "(int capacity){items=new " << first
            << "[capacity];count=0;}"
            << "void EnsureCapacity(int needed){if(needed<=items.length)return;"
            << "int next=items.length;if(next<4)next=4;"
            << "while(next<needed){next=next*2;}"
            << first << "[] grown=new " << first << "[next];int i=0;"
            << "while(i<count){grown[i]=items[i];i=i+1;}items=grown;return;}"
            << "int Count(){return count;}int Capacity(){return items.length;}"
            << "void Add(" << first << " value){EnsureCapacity(count+1);"
            << "items[count]=value;count=count+1;return;}"
            << "void Insert(int index," << first << " value){EnsureCapacity(count+1);"
            << "int i=count;while(i>index){items[i]=items[i-1];i=i-1;}"
            << "items[index]=value;count=count+1;return;}"
            << "void RemoveAt(int index){int i=index;while(i<count-1){"
            << "items[i]=items[i+1];i=i+1;}count=count-1;return;}"
            << "bool Remove(" << first << " value){int i=IndexOf(value);"
            << "if(i<0)return false;RemoveAt(i);return true;}"
            << "int IndexOf(" << first << " value){int i=0;while(i<count){"
            << "if(items[i]==value)return i;i=i+1;}return -1;}"
            << "bool Contains(" << first << " value){return IndexOf(value)>=0;}"
            << first << " Get(int index){return items[index];}"
            << "void Set(int index," << first << " value){items[index]=value;return;}"
            << "void Clear(){count=0;return;}" << enumerator
            << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "class " << enumerator << "{" << generatedName
            << " owner;int index;" << enumerator << "(" << generatedName
            << " value){owner=value;index=-1;}"
            << "bool MoveNext(){index=index+1;return index<owner.Count();}"
            << first << " Current(){return owner.Get(index);}}";
    } else if (name == "Queue") {
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] items;int head;int tail;int count;"
            << generatedName << "(){items=new " << first
            << "[4];head=0;tail=0;count=0;}"
            << generatedName << "(int capacity){items=new " << first
            << "[capacity];head=0;tail=0;count=0;}"
            << "void EnsureCapacity(int needed){if(needed<=items.length)return;"
            << "int next=items.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << first << "[] grown=new " << first << "[next];int i=0;"
            << "while(i<count){grown[i]=items[(head+i)%items.length];i=i+1;}"
            << "items=grown;head=0;tail=count;return;}"
            << "int Count(){return count;}int Capacity(){return items.length;}"
            << "void Enqueue(" << first << " value){EnsureCapacity(count+1);"
            << "items[tail]=value;tail=(tail+1)%items.length;count=count+1;return;}"
            << first << " Dequeue(){" << first << " value=items[head];"
            << "head=(head+1)%items.length;count=count-1;return value;}"
            << first << " Peek(){return items[head];}"
            << first << " Get(int index){return items[(head+index)%items.length];}"
            << "void Clear(){head=0;tail=0;count=0;return;}" << enumerator
            << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "class " << enumerator << "{" << generatedName
            << " owner;int index;" << enumerator << "(" << generatedName
            << " value){owner=value;index=-1;}bool MoveNext(){index=index+1;"
            << "return index<owner.Count();}" << first
            << " Current(){return owner.Get(index);}}";
    } else if (name == "Stack") {
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] items;int count;" << generatedName << "(){items=new "
            << first << "[4];count=0;}" << generatedName
            << "(int capacity){items=new " << first << "[capacity];count=0;}"
            << "void EnsureCapacity(int needed){if(needed<=items.length)return;"
            << "int next=items.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << first << "[] grown=new " << first << "[next];int i=0;"
            << "while(i<count){grown[i]=items[i];i=i+1;}items=grown;return;}"
            << "int Count(){return count;}int Capacity(){return items.length;}"
            << "void Push(" << first << " value){EnsureCapacity(count+1);"
            << "items[count]=value;count=count+1;return;}"
            << first << " Pop(){count=count-1;return items[count];}"
            << first << " Peek(){return items[count-1];}"
            << first << " Get(int index){return items[count-1-index];}"
            << "void Clear(){count=0;return;}" << enumerator
            << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "class " << enumerator << "{" << generatedName
            << " owner;int index;" << enumerator << "(" << generatedName
            << " value){owner=value;index=-1;}bool MoveNext(){index=index+1;"
            << "return index<owner.Count();}" << first
            << " Current(){return owner.Get(index);}}";
    } else if (name == "Nullable") {
        source << "struct " << generatedName
            << "{bool hasValue;" << first << " value;"
            << generatedName << "(" << first
            << " initial){value=initial;hasValue=true;}"
            << "bool HasValue(){return hasValue;}"
            << first << " Value(){return value;}"
            << first << " GetValueOrDefault(){return value;}"
            << "void Clear(){hasValue=false;return;}}";
    } else if (name == "Box") {
        source << "class " << generatedName
            << "{" << first << " value;"
            << generatedName << "(" << first
            << " initial){value=initial;}"
            << first << " Value(){return value;}}";
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
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] values;int count;" << generatedName << "(){values=new "
            << first << "[4];count=0;}" << generatedName
            << "(int capacity){values=new " << first << "[capacity];count=0;}"
            << "void EnsureCapacity(int needed){if(needed<=values.length)return;"
            << "int next=values.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << first << "[] grown=new " << first << "[next];int i=0;"
            << "while(i<count){grown[i]=values[i];i=i+1;}values=grown;return;}"
            << "int Count(){return count;}"
            << "bool Contains(" << first
            << " value){int i=0;while(i<count){"
            << "if(values[i]==value)return true;i=i+1;}return false;}"
            << "bool Add(" << first
            << " value){if(Contains(value))return false;EnsureCapacity(count+1);"
            << "values[count]=value;count=count+1;return true;}"
            << "bool Remove(" << first << " value){int i=0;while(i<count){"
            << "if(values[i]==value){int j=i;while(j<count-1){values[j]=values[j+1];"
            << "j=j+1;}count=count-1;return true;}i=i+1;}return false;}"
            << first << " Get(int index){return values[index];}"
            << "void Clear(){count=0;return;}" << enumerator
            << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "class " << enumerator << "{" << generatedName
            << " owner;int index;" << enumerator << "(" << generatedName
            << " value){owner=value;index=-1;}bool MoveNext(){index=index+1;"
            << "return index<owner.Count();}" << first
            << " Current(){return owner.Get(index);}}";
    } else if (name == "Dictionary") {
        if (arguments.size() != 2) return {};
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] keys;" << second << "[] values;int count;" << generatedName
            << "(){keys=new " << first << "[4];values=new " << second
            << "[4];count=0;}" << generatedName
            << "(int capacity){keys=new " << first
            << "[capacity];values=new " << second
            << "[capacity];count=0;}"
            << "void EnsureCapacity(int needed){if(needed<=keys.length)return;"
            << "int next=keys.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << first << "[] grownKeys=new " << first << "[next];"
            << second << "[] grownValues=new " << second << "[next];int i=0;"
            << "while(i<count){grownKeys[i]=keys[i];grownValues[i]=values[i];i=i+1;}"
            << "keys=grownKeys;values=grownValues;return;}"
            << "int Count(){return count;}"
            << "int Find(" << first
            << " key){int i=0;while(i<count){if(keys[i]==key)return i;"
            << "i=i+1;}return -1;}"
            << "bool ContainsKey(" << first
            << " key){return Find(key)>=0;}"
            << "void Put(" << first << " key," << second
            << " value){int i=Find(key);if(i>=0){values[i]=value;return;}"
            << "EnsureCapacity(count+1);"
            << "keys[count]=key;values[count]=value;count=count+1;return;}"
            << "void Add(" << first << " key," << second
            << " value){Put(key,value);return;}"
            << second << " Get(" << first
            << " key){return values[Find(key)];}"
            << second << " GetAt(int index){return values[index];}"
            << "bool Remove(" << first << " key){int i=Find(key);if(i<0)return false;"
            << "while(i<count-1){keys[i]=keys[i+1];values[i]=values[i+1];i=i+1;}"
            << "count=count-1;return true;}"
            << "void Clear(){count=0;return;}" << enumerator
            << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "class " << enumerator << "{" << generatedName
            << " owner;int index;" << enumerator << "(" << generatedName
            << " value){owner=value;index=-1;}bool MoveNext(){index=index+1;"
            << "return index<owner.Count();}" << second
            << " Current(){return owner.GetAt(index);}}";
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
        if (token.kind == syntax::SyntaxKind::IdentifierToken &&
            token.text == "where") {
            while (index + 1 < tokens.size() &&
                   tokens[index + 1].kind != syntax::SyntaxKind::OpenBraceToken &&
                   tokens[index + 1].kind != syntax::SyntaxKind::SemicolonToken) {
                ++index;
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
        for (std::size_t index = 0; index < pendingMembers_.size(); ++index) {
            materializeMember(pendingMembers_[index], queue);
            for (std::size_t queueIndex = 0; queueIndex < queue.size(); ++queueIndex) {
                auto* unit = queue[queueIndex];
                if (!unit || !processed_.insert(unit).second) continue;
                rewriteUnit(*unit, queue);
            }
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
                        declaration.constraints,
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
                        declaration.constraints,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Struct);
                }
                for (const auto& declaration : unit->syntaxTree->interfaces) {
                    if (declaration.typeParameters.empty()) continue;
                    addTypeTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.constraints,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Interface);
                }
                for (const auto& declaration : unit->syntaxTree->delegates) {
                    if (declaration.typeParameters.empty()) continue;
                    addTypeTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.constraints,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Delegate);
                }
                for (const auto& declaration : unit->syntaxTree->functions) {
                    if (declaration.typeParameters.empty()) continue;
                    addFunctionTemplate(moduleName, declaration, unit);
                }
                for (const auto& declaration : unit->syntaxTree->classes) {
                    for (const auto& method : declaration.methods) {
                        if (!method.typeParameters.empty()) {
                            addMemberTemplate(
                                moduleName,
                                declaration.identifierToken.text,
                                method,
                                unit);
                        }
                    }
                }
                for (const auto& declaration : unit->syntaxTree->structs) {
                    for (const auto& method : declaration.methods) {
                        if (!method.typeParameters.empty()) {
                            addMemberTemplate(
                                moduleName,
                                declaration.identifierToken.text,
                                method,
                                unit);
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
        const std::vector<syntax::GenericConstraintClauseSyntax>& constraints,
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
        declaration.constraints = nativeGenericConstraints(constraints);
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
        const syntax::FunctionDeclarationSyntax& function,
        const ParsedUnit* unit) {
        NativeGenericTemplate declaration;
        declaration.kind = NativeGenericTemplate::Kind::Function;
        declaration.moduleName = moduleName;
        declaration.name = function.identifierToken.text;
        declaration.sourceName = unit->source->name();
        declaration.span = function.span();
        declaration.unit = unit;
        for (const auto& parameter : function.typeParameters) {
            declaration.parameters.push_back(parameter.text);
        }
        for (const auto& parameter : function.parameters) {
            declaration.parameterTypes.push_back(
                nativeGenericTypeText(parameter.type));
        }
        declaration.constraints = nativeGenericConstraints(function.constraints);
        auto& table = registry_.functions[moduleName];
        if (!table.emplace(function.identifierToken.text,
                std::move(declaration)).second) {
            result_.diagnostics.report(
                "RS8502",
                "duplicate generic function template '" +
                    function.identifierToken.text + "'",
                function.span(),
                diagnostics::DiagnosticSeverity::Error,
                unit->source->name());
            modules_[moduleName].invalid = true;
        }
    }

    void addMemberTemplate(
        const std::string& moduleName,
        const std::string& ownerName,
        const syntax::FunctionDeclarationSyntax& function,
        const ParsedUnit* unit) {
        NativeGenericTemplate declaration;
        declaration.kind = NativeGenericTemplate::Kind::Function;
        declaration.moduleName = moduleName;
        declaration.ownerName = ownerName;
        declaration.name = function.identifierToken.text;
        declaration.sourceName = unit->source->name();
        declaration.span = function.span();
        declaration.unit = unit;
        for (const auto& parameter : function.typeParameters) {
            declaration.parameters.push_back(parameter.text);
        }
        for (const auto& parameter : function.parameters) {
            declaration.parameterTypes.push_back(
                nativeGenericTypeText(parameter.type));
        }
        declaration.constraints = nativeGenericConstraints(function.constraints);
        registry_.members[moduleName][function.identifierToken.text]
            .push_back(std::move(declaration));
    }

    const NativeGenericTemplate* resolveMember(
        const std::string& moduleName,
        const std::string& name,
        const std::string& receiverType,
        text::TextSpan span,
        const std::string& sourceName) {
        std::vector<const NativeGenericTemplate*> candidates;
        const auto append = [&](const std::string& candidateModule) {
            const auto module = registry_.members.find(candidateModule);
            if (module == registry_.members.end()) return;
            const auto found = module->second.find(name);
            if (found == module->second.end()) return;
            for (const auto& value : found->second) {
                if (receiverType.empty() || receiverType == value.ownerName) {
                    candidates.push_back(&value);
                }
            }
        };
        append(moduleName);
        const auto module = modules_.find(moduleName);
        if (module != modules_.end()) {
            for (const auto& imported : module->second.imports) append(imported);
        }
        if (candidates.size() == 1) return candidates.front();
        if (candidates.size() > 1) {
            result_.diagnostics.report(
                "RS8522",
                "generic member method '" + name + "' is ambiguous",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[moduleName].invalid = true;
        }
        return nullptr;
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
        return nativeGenericTypeText(type);
    }

    std::string expressionType(const syntax::ExpressionSyntax& expression) const {
        switch (expression.kind()) {
        case syntax::SyntaxKind::NameExpression: {
            const auto& name = static_cast<const
                syntax::NameExpressionSyntax&>(expression).identifierToken.text;
            const auto found = localTypes_.find(name);
            return found == localTypes_.end() ? std::string{} : found->second;
        }
        case syntax::SyntaxKind::LiteralExpression: {
            const auto kind = static_cast<const
                syntax::LiteralExpressionSyntax&>(expression).literalToken.kind;
            if (kind == syntax::SyntaxKind::IntegerLiteralToken) return "int";
            if (kind == syntax::SyntaxKind::FloatLiteralToken) return "double";
            if (kind == syntax::SyntaxKind::StringLiteralToken) return "string";
            if (kind == syntax::SyntaxKind::TrueKeyword ||
                kind == syntax::SyntaxKind::FalseKeyword) return "bool";
            return {};
        }
        case syntax::SyntaxKind::ParenthesizedExpression:
            return expressionType(*static_cast<const
                syntax::ParenthesizedExpressionSyntax&>(expression).expression);
        case syntax::SyntaxKind::NewObjectExpression:
            return typeText(static_cast<const
                syntax::NewObjectExpressionSyntax&>(expression).type);
        case syntax::SyntaxKind::NewArrayExpression:
            return typeText(static_cast<const
                syntax::NewArrayExpressionSyntax&>(expression).elementType) + "[]";
        default:
            return {};
        }
    }

    std::optional<std::vector<std::string>> inferArguments(
        const NativeGenericTemplate& declaration,
        const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& arguments) const {
        if (declaration.parameterTypes.size() != arguments.size()) {
            return std::nullopt;
        }
        std::map<std::string, std::string> inferred;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto actual = expressionType(*arguments[index]);
            if (actual.empty()) return std::nullopt;
            const auto& formal = declaration.parameterTypes[index];
            for (const auto& parameter : declaration.parameters) {
                bool matched = false;
                std::string candidate;
                if (formal == parameter) {
                    matched = true;
                    candidate = actual;
                } else if (formal == parameter + "[]" &&
                           actual.size() > 2 &&
                           actual.substr(actual.size() - 2) == "[]") {
                    matched = true;
                    candidate = actual.substr(0, actual.size() - 2);
                }
                if (!matched) continue;
                const auto previous = inferred.find(parameter);
                if (previous != inferred.end() && previous->second != candidate) {
                    return std::nullopt;
                }
                inferred[parameter] = std::move(candidate);
            }
        }
        std::vector<std::string> result;
        for (const auto& parameter : declaration.parameters) {
            const auto found = inferred.find(parameter);
            if (found == inferred.end()) return std::nullopt;
            result.push_back(found->second);
        }
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

        if (declaration && !validateConstraints(
                *declaration, arguments, span, sourceName, consumerModule)) {
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

    struct ConcreteTypeFacts {
        bool known = false;
        bool valueType = false;
        bool referenceType = false;
        bool defaultConstructor = false;
    };

    ConcreteTypeFacts concreteTypeFacts(const std::string& name) const {
        static const std::set<std::string> primitiveValues = {
            "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
            "long", "ulong", "float", "double", "char", "fixed32",
            "fixed64"};
        if (primitiveValues.find(name) != primitiveValues.end()) {
            return {true, true, false, true};
        }
        if (name == "string" || name == "object" ||
            (name.size() > 2 && name.substr(name.size() - 2) == "[]")) {
            return {true, false, true, false};
        }
        for (const auto& unit : units_) {
            for (const auto& value : unit->syntaxTree->structs) {
                if (value.identifierToken.text == name) {
                    return {true, true, false, true};
                }
            }
            for (const auto& value : unit->syntaxTree->enums) {
                if (value.identifierToken.text == name) {
                    return {true, true, false, true};
                }
            }
            for (const auto& value : unit->syntaxTree->classes) {
                if (value.identifierToken.text != name) continue;
                bool defaultConstructor = value.constructors.empty();
                for (const auto& constructor : value.constructors) {
                    defaultConstructor = defaultConstructor ||
                        constructor.parameters.empty();
                }
                return {true, false, true, defaultConstructor};
            }
            for (const auto& value : unit->syntaxTree->interfaces) {
                if (value.identifierToken.text == name) {
                    return {true, false, true, false};
                }
            }
            for (const auto& value : unit->syntaxTree->delegates) {
                if (value.identifierToken.text == name) {
                    return {true, false, true, false};
                }
            }
        }
        return {};
    }

    bool validateConstraints(
        const NativeGenericTemplate& declaration,
        const std::vector<std::string>& arguments,
        text::TextSpan span,
        const std::string& sourceName,
        const std::string& consumerModule) {
        bool valid = true;
        for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
            const auto found = declaration.constraints.find(
                declaration.parameters[index]);
            if (found == declaration.constraints.end()) continue;
            const auto facts = concreteTypeFacts(arguments[index]);
            for (const auto& constraint : found->second) {
                bool satisfied = true;
                if (constraint == "class") satisfied = facts.referenceType;
                else if (constraint == "struct") satisfied = facts.valueType;
                else if (constraint == "new()") satisfied = facts.defaultConstructor;
                else continue;
                if (satisfied) continue;
                result_.diagnostics.report(
                    "RS8530",
                    "type argument '" + arguments[index] +
                        "' does not satisfy constraint '" + constraint +
                        "' for '" + declaration.parameters[index] + "'",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                valid = false;
            }
        }
        return valid;
    }

    std::string requestMember(
        const std::string& consumerModule,
        const NativeGenericTemplate& declaration,
        const std::vector<std::string>& arguments,
        text::TextSpan span,
        const std::string& sourceName) {
        if (arguments.size() != declaration.parameters.size()) {
            result_.diagnostics.report(
                "RS8501",
                "generic arity mismatch for member '" + declaration.name + "'",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return declaration.name;
        }
        if (!validateConstraints(
                declaration, arguments, span, sourceName, consumerModule)) {
            return declaration.name;
        }
        const auto generated = nativeGenericGeneratedName(
            declaration.name, arguments);
        const auto key = declaration.moduleName + "::member:" +
            declaration.ownerName + "::" + generated;
        if (specializations_.insert(key).second) {
            pendingMembers_.push_back(PendingNativeGenericMember{
                &declaration, arguments, generated, span, sourceName});
            result_.nativeGenericInstantiations.push_back(
                LanguageGenericInstantiation{
                    declaration.ownerName + "." + declaration.name,
                    arguments,
                    generated});
        }
        return generated;
    }

    void materializeMember(
        const PendingNativeGenericMember& pending,
        std::vector<ParsedUnit*>& queue) {
        if (!pending.declaration || !pending.declaration->unit) return;
        const auto body = nativeSpecializedDeclarationSource(
            *pending.declaration,
            pending.arguments,
            pending.generatedName,
            result_.diagnostics);
        const auto wrapper = "class __RsGenericMemberContainer{" + body + "}";
        text::SourceText source(wrapper, "$generated/generic-member.rs");
        diagnostics::DiagnosticBag diagnostics;
        syntax::Parser parser(source, diagnostics);
        auto parsed = parser.parseCompilationUnit();
        if (diagnostics.hasErrors() || parsed.classes.empty() ||
            parsed.classes.front().methods.empty()) {
            result_.diagnostics.report(
                "RS8523",
                "generic member specialization '" + pending.generatedName +
                    "' could not be generated",
                pending.requestSpan,
                diagnostics::DiagnosticSeverity::Error,
                pending.sourceName);
            modules_[pending.declaration->moduleName].invalid = true;
            return;
        }
        auto method = std::move(parsed.classes.front().methods.front());
        auto* ownerUnit = const_cast<ParsedUnit*>(pending.declaration->unit);
        for (auto& declaration : ownerUnit->syntaxTree->classes) {
            if (declaration.identifierToken.text !=
                pending.declaration->ownerName) continue;
            declaration.methods.push_back(std::move(method));
            rewriteFunction(
                declaration.methods.back(),
                pending.declaration->moduleName,
                pending.declaration->sourceName,
                queue);
            return;
        }
        for (auto& declaration : ownerUnit->syntaxTree->structs) {
            if (declaration.identifierToken.text !=
                pending.declaration->ownerName) continue;
            declaration.methods.push_back(std::move(method));
            rewriteFunction(
                declaration.methods.back(),
                pending.declaration->moduleName,
                pending.declaration->sourceName,
                queue);
            return;
        }
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
            for (auto& argument : call.arguments) {
                rewriteExpression(*argument, moduleName, sourceName, queue);
            }
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
            } else if (const auto* declaration = resolve(
                           moduleName,
                           call.identifierToken.text,
                           true,
                           call.span(),
                           sourceName)) {
                const auto inferred = inferArguments(*declaration, call.arguments);
                if (inferred) {
                    call.identifierToken.text = request(
                        moduleName,
                        call.identifierToken.text,
                        *inferred,
                        true,
                        call.span(),
                        sourceName,
                        queue);
                } else {
                    result_.diagnostics.report(
                        "RS8520",
                        "generic type arguments for '" +
                            call.identifierToken.text +
                            "' could not be inferred",
                        call.span(),
                        diagnostics::DiagnosticSeverity::Error,
                        sourceName);
                    modules_[moduleName].invalid = true;
                }
            }
            return;
        }
        case syntax::SyntaxKind::MemberCallExpression: {
            auto& call = static_cast<syntax::MemberCallExpressionSyntax&>(expression);
            rewriteExpression(*call.receiver, moduleName, sourceName, queue);
            for (auto& argument : call.arguments) {
                rewriteExpression(*argument, moduleName, sourceName, queue);
            }
            const auto receiverType = expressionType(*call.receiver);
            const auto* declaration = resolveMember(
                moduleName,
                call.nameToken.text,
                receiverType,
                call.span(),
                sourceName);
            if (call.lessToken) {
                if (declaration) {
                    std::vector<std::string> arguments;
                    for (auto& argument : call.typeArguments) {
                        rewriteType(argument, moduleName, sourceName, queue);
                        arguments.push_back(typeText(argument));
                    }
                    call.nameToken.text = requestMember(
                        moduleName, *declaration, arguments,
                        call.span(), sourceName);
                }
                call.lessToken.reset();
                call.typeArguments.clear();
                call.typeArgumentCommaTokens.clear();
                call.greaterToken.reset();
            } else if (declaration) {
                const auto inferred = inferArguments(*declaration, call.arguments);
                if (inferred) {
                    call.nameToken.text = requestMember(
                        moduleName, *declaration, *inferred,
                        call.span(), sourceName);
                }
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
        case syntax::SyntaxKind::YieldBreakStatement:
            return;
        case syntax::SyntaxKind::EventSubscriptionStatement: {
            auto& value = static_cast<
                syntax::EventSubscriptionStatementSyntax&>(statement);
            if (value.receiver) rewriteExpression(
                *value.receiver, moduleName, sourceName, queue);
            rewriteExpression(
                *value.handler, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::VariableDeclarationStatement: {
            auto& value = static_cast<syntax::VariableDeclarationStatementSyntax&>(statement);
            rewriteType(value.type, moduleName, sourceName, queue);
            localTypes_[value.identifierToken.text] = typeText(value.type);
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
        const auto previousLocalTypes = localTypes_;
        localTypes_.clear();
        rewriteType(function.returnType, moduleName, sourceName, queue);
        for (auto& parameter : function.parameters) {
            rewriteParameter(parameter, moduleName, sourceName, queue);
            localTypes_[parameter.identifierToken.text] = typeText(parameter.type);
        }
        rewriteStatement(function.body, moduleName, sourceName, queue);
        localTypes_ = previousLocalTypes;
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
            if (sequence.resultType) {
                rewriteType(
                    *sequence.resultType,
                    moduleName,
                    sourceName,
                    queue);
            }
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
            if (!declaration.typeParameters.empty()) continue;
            for (auto& method : declaration.methods) {
                rewriteType(method.returnType, moduleName, sourceName, queue);
                for (auto& parameter : method.parameters) rewriteParameter(parameter, moduleName, sourceName, queue);
            }
        }
        for (auto& declaration : unit.syntaxTree->delegates) {
            if (!declaration.typeParameters.empty()) continue;
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
    std::map<std::string, std::string> localTypes_;
    std::vector<PendingNativeGenericMember> pendingMembers_;
};

void specializeNativeGenerics(
    std::vector<std::unique_ptr<ParsedUnit>>& units,
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    NativeGenericSpecializer specializer(units, modules, result);
    specializer.run();
}
