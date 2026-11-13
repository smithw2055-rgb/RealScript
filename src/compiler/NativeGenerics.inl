// Native bounded generic specialization for Phase 18.
// Included inside compiler::anonymous namespace after ParsedUnit/ModuleWork.

struct NativeGenericTemplate {
    enum class Kind { Class, Struct, Interface, Delegate, Function };
    Kind kind = Kind::Class;
    std::string moduleName;
    std::string ownerName;
    std::size_t ownerArity = 0;
    std::string name;
    std::string sourceName;
    text::TextSpan span;
    std::vector<std::string> parameters;
    std::vector<std::string> parameterTypes;
    std::map<std::string, std::vector<std::string>> constraints;
    std::vector<syntax::GenericConstraintClauseSyntax> rawConstraints;
    const ParsedUnit* unit = nullptr;
    const ParsedUnit* sourceUnit = nullptr;
};

using PartialMethodOriginMap = std::map<
    std::tuple<std::string, std::string, std::string, std::size_t, std::size_t>,
    const ParsedUnit*>;

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

std::string nativeGenericTypeKey(const std::string& name, std::size_t arity) {
    return name + "#" + std::to_string(arity);
}

std::string nativeGenericGeneratedName(
    const std::string& name,
    const std::vector<std::string>& arguments) {
    auto result = (name == "ObservableCollection")
        ? std::string("ObservableList")
        : name;
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
    if (name == "List" || name == "ObservableList" || name == "ObservableCollection" ||
        name == "Queue" || name == "Stack" ||
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
    if (name == "ObservableList" || name == "ObservableCollection") {
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first << "[] items;"
            << generatedName << "(){items=new " << first << "[0];}"
            << generatedName << "(int capacity){items=new " << first << "[0];items.reserve(capacity);}"
            << "int Count(){return items.length;}int Length(){return items.length;}"
            << "bool IsEmpty(){return items.length==0;}"
            << "void Add(" << first << " value){items.push(value);return;}"
            << "void Push(" << first << " value){items.push(value);return;}"
            << "void push(" << first << " value){items.push(value);return;}"
            << "void Insert(int index," << first << " value){items.insert(index,value);return;}"
            << "void insert(int index," << first << " value){items.insert(index,value);return;}"
            << "void RemoveAt(int index){items.removeAt(index);return;}"
            << "void removeAt(int index){items.removeAt(index);return;}"
            << "void Move(int oldIndex,int newIndex){int n=items.length;if(oldIndex<0||oldIndex>=n||newIndex<0||newIndex>=n||oldIndex==newIndex)return;"
            << first << " moved=items[oldIndex];items.removeAt(oldIndex);items.insert(newIndex,moved);return;}"
            << "bool Remove(" << first << " value){int i=items.IndexOf(value);"
            << "if(i<0)return false;items.removeAt(i);return true;}"
            << "int IndexOf(" << first << " value){return items.IndexOf(value);}"
            << "int LastIndexOf(" << first << " value){return items.LastIndexOf(value);}"
            << "bool Contains(" << first << " value){return items.Contains(value);}"
            << first << " Get(int index){return items[index];}"
            << "void Set(int index," << first << " value){items[index]=value;return;}"
            << first << " First(){return items[0];}"
            << first << " Last(){return items[items.length-1];}"
            << "void Reverse(){items.Reverse();return;}"
            << "void Sort(){items.Sort();return;}"
            << first << "[] AsArray(){return items;}"
            << first << "[] ToArray(){" << first << "[] arr=new " << first
            << "[items.length];int i=0;while(i<items.length){arr[i]=items[i];i=i+1;}return arr;}"
            << "void Clear(){items.clear();return;}"
            << "void clear(){items.clear();return;}"
            << enumerator << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "class " << enumerator << "{" << generatedName
            << " owner;int index;" << enumerator << "(" << generatedName
            << " value){owner=value;index=-1;}"
            << "bool MoveNext(){index=index+1;return index<owner.Count();}"
            << first << " Current(){return owner.Get(index);}}";
    } else if (name == "List") {
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
            << "int Count(){return count;}int Length(){return count;}int Capacity(){return items.length;}"
            << "bool IsEmpty(){return count==0;}"
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
            << "int LastIndexOf(" << first << " value){int i=count-1;while(i>=0){"
            << "if(items[i]==value)return i;i=i-1;}return -1;}"
            << "bool Contains(" << first << " value){return IndexOf(value)>=0;}"
            << first << " Get(int index){return items[index];}"
            << "void Set(int index," << first << " value){items[index]=value;return;}"
            << first << " First(){return items[0];}"
            << first << " Last(){return items[count-1];}"
            << "void Reverse(){int l=0;int r=count-1;while(l<r){"
            << first << " tmp=items[l];items[l]=items[r];items[r]=tmp;"
            << "l=l+1;r=r-1;}return;}"
            << "void Sort(){__rs_array_sort_range(items,count);return;}"
            << first << "[] ToArray(){" << first << "[] arr=new " << first
            << "[count];int i=0;while(i<count){arr[i]=items[i];i=i+1;}return arr;}"
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
            << "bool IsEmpty(){return count==0;}"
            << "void Enqueue(" << first << " value){EnsureCapacity(count+1);"
            << "items[tail]=value;tail=(tail+1)%items.length;count=count+1;return;}"
            << first << " Dequeue(){" << first << " value=items[head];"
            << "head=(head+1)%items.length;count=count-1;return value;}"
            << first << " Peek(){return items[head];}"
            << first << " Get(int index){return items[(head+index)%items.length];}"
            << "bool Contains(" << first << " value){int i=0;while(i<count){"
            << "if(items[(head+i)%items.length]==value)return true;i=i+1;}return false;}"
            << first << "[] ToArray(){" << first << "[] arr=new " << first
            << "[count];int i=0;while(i<count){arr[i]=items[(head+i)%items.length];i=i+1;}return arr;}"
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
            << "bool IsEmpty(){return count==0;}"
            << "void Push(" << first << " value){EnsureCapacity(count+1);"
            << "items[count]=value;count=count+1;return;}"
            << first << " Pop(){count=count-1;return items[count];}"
            << first << " Peek(){return items[count-1];}"
            << first << " Get(int index){return items[count-1-index];}"
            << "bool Contains(" << first << " value){int i=0;while(i<count){"
            << "if(items[i]==value)return true;i=i+1;}return false;}"
            << first << "[] ToArray(){" << first << "[] arr=new " << first
            << "[count];int i=0;while(i<count){arr[i]=items[count-1-i];i=i+1;}return arr;}"
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
            << "[] values;int[] buckets;int[] nexts;int count;" << generatedName
            << "(){values=new " << first << "[4];buckets=new int[8];nexts=new int[4];count=0;}"
            << generatedName << "(int capacity){int cap=capacity;if(cap<4)cap=4;"
            << "int bcap=8;while(bcap<cap*2){bcap=bcap*2;}"
            << "values=new " << first << "[cap];buckets=new int[bcap];nexts=new int[cap];count=0;}"
            << "void Rehash(){int bcap=buckets.length;int bi=0;while(bi<bcap){buckets[bi]=0;bi=bi+1;}"
            << "int i=0;while(i<count){int h=__rs_stable_hash(values[i]);if(h<0)h=0-h;"
            << "int b=h%bcap;nexts[i]=buckets[b];buckets[b]=i+1;i=i+1;}return;}"
            << "void EnsureCapacity(int needed){if(needed<=values.length)return;"
            << "int next=values.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << "int bcap=8;while(bcap<next*2){bcap=bcap*2;}"
            << first << "[] grown=new " << first << "[next];int i=0;"
            << "while(i<count){grown[i]=values[i];i=i+1;}values=grown;"
            << "buckets=new int[bcap];nexts=new int[next];Rehash();return;}"
            << "int Count(){return count;}"
            << "bool IsEmpty(){return count==0;}"
            << "int FindIndex(" << first << " value){if(count==0)return -1;"
            << "int bcap=buckets.length;if(bcap==0)return -1;"
            << "int h=__rs_stable_hash(value);if(h<0)h=0-h;"
            << "int cur=buckets[h%bcap]-1;while(cur>=0){"
            << "if(values[cur]==value)return cur;cur=nexts[cur]-1;}return -1;}"
            << "bool Contains(" << first << " value){return FindIndex(value)>=0;}"
            << "bool Add(" << first
            << " value){if(Contains(value))return false;EnsureCapacity(count+1);"
            << "int idx=count;values[idx]=value;count=count+1;"
            << "int bcap=buckets.length;int h=__rs_stable_hash(value);if(h<0)h=0-h;"
            << "int b=h%bcap;nexts[idx]=buckets[b];buckets[b]=idx+1;return true;}"
            << "bool Remove(" << first << " value){int i=FindIndex(value);if(i<0)return false;"
            << "int j=i;while(j<count-1){values[j]=values[j+1];j=j+1;}"
            << "count=count-1;Rehash();return true;}"
            << first << " Get(int index){return values[index];}"
            << first << "[] ToArray(){" << first << "[] arr=new " << first
            << "[count];int i=0;while(i<count){arr[i]=values[i];i=i+1;}return arr;}"
            << "void Clear(){count=0;values=new " << first << "[4];buckets=new int[8];nexts=new int[4];return;}"
            << enumerator << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "class " << enumerator << "{" << generatedName
            << " owner;int index;" << enumerator << "(" << generatedName
            << " value){owner=value;index=-1;}bool MoveNext(){index=index+1;"
            << "return index<owner.Count();}" << first
            << " Current(){return owner.Get(index);}}";
    } else if (name == "Dictionary") {
        if (arguments.size() != 2) return {};
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] keys;" << second << "[] values;int[] buckets;int[] nexts;int count;"
            << generatedName << "(){keys=new " << first << "[4];values=new " << second
            << "[4];buckets=new int[8];nexts=new int[4];count=0;}" << generatedName
            << "(int capacity){int cap=capacity;if(cap<4)cap=4;"
            << "int bcap=8;while(bcap<cap*2){bcap=bcap*2;}"
            << "keys=new " << first << "[cap];values=new " << second
            << "[cap];buckets=new int[bcap];nexts=new int[cap];count=0;}"
            << "void Rehash(){int bcap=buckets.length;int bi=0;while(bi<bcap){buckets[bi]=0;bi=bi+1;}"
            << "int i=0;while(i<count){int h=__rs_stable_hash(keys[i]);if(h<0)h=0-h;"
            << "int b=h%bcap;nexts[i]=buckets[b];buckets[b]=i+1;i=i+1;}return;}"
            << "void EnsureCapacity(int needed){if(needed<=keys.length)return;"
            << "int next=keys.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << "int bcap=8;while(bcap<next*2){bcap=bcap*2;}"
            << first << "[] grownKeys=new " << first << "[next];"
            << second << "[] grownValues=new " << second << "[next];int i=0;"
            << "while(i<count){grownKeys[i]=keys[i];grownValues[i]=values[i];i=i+1;}"
            << "keys=grownKeys;values=grownValues;buckets=new int[bcap];nexts=new int[next];Rehash();return;}"
            << "int Count(){return count;}"
            << "bool IsEmpty(){return count==0;}"
            << "int Find(" << first << " key){if(count==0)return -1;"
            << "int bcap=buckets.length;if(bcap==0)return -1;"
            << "int h=__rs_stable_hash(key);if(h<0)h=0-h;"
            << "int cur=buckets[h%bcap]-1;while(cur>=0){"
            << "if(keys[cur]==key)return cur;cur=nexts[cur]-1;}return -1;}"
            << "bool ContainsKey(" << first
            << " key){return Find(key)>=0;}"
            << "bool ContainsValue(" << second
            << " value){int i=0;while(i<count){if(values[i]==value)return true;"
            << "i=i+1;}return false;}"
            << "bool TryGetValue(" << first << " key,out " << second
            << " value){int i=Find(key);if(i<0){" << second
            << "[] empty=new " << second << "[1];value=empty[0];return false;}value=values[i];return true;}"
            << second << " GetOrDefault(" << first << " key," << second
            << " fallback){int i=Find(key);if(i<0)return fallback;return values[i];}"
            << first << "[] Keys(){" << first << "[] arr=new " << first
            << "[count];int i=0;while(i<count){arr[i]=keys[i];i=i+1;}return arr;}"
            << second << "[] Values(){" << second << "[] arr=new " << second
            << "[count];int i=0;while(i<count){arr[i]=values[i];i=i+1;}return arr;}"
            << "void Put(" << first << " key," << second
            << " value){int i=Find(key);if(i>=0){values[i]=value;return;}"
            << "EnsureCapacity(count+1);"
            << "int idx=count;keys[idx]=key;values[idx]=value;count=count+1;"
            << "int bcap=buckets.length;int h=__rs_stable_hash(key);if(h<0)h=0-h;"
            << "int b=h%bcap;nexts[idx]=buckets[b];buckets[b]=idx+1;return;}"
            << "void Add(" << first << " key," << second
            << " value){Put(key,value);return;}"
            << second << " Get(" << first
            << " key){return values[Find(key)];}"
            << second << " GetAt(int index){return values[index];}"
            << "bool Remove(" << first << " key){int i=Find(key);if(i<0)return false;"
            << "while(i<count-1){keys[i]=keys[i+1];values[i]=values[i+1];i=i+1;}"
            << "count=count-1;Rehash();return true;}"
            << "void Clear(){count=0;keys=new " << first << "[4];values=new " << second << "[4];buckets=new int[8];nexts=new int[4];return;}"
            << enumerator << " GetEnumerator(){return new " << enumerator << "(this);}}"
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
    const auto* srcUnit = declaration.sourceUnit ? declaration.sourceUnit : declaration.unit;
    if (!srcUnit || !srcUnit->source ||
        declaration.parameters.size() != arguments.size() ||
        declaration.span.end() > srcUnit->source->content().size()) {
        return {};
    }
    const auto fragment = srcUnit->source->content().substr(
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
                   tokens[index + 1].kind != syntax::SyntaxKind::SemicolonToken &&
                   tokens[index + 1].kind != syntax::SyntaxKind::ArrowToken) {
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
        BuildResult& result,
        const PartialMethodOriginMap* partialMethodOrigins = nullptr)
        : units_(units),
          modules_(modules),
          result_(result),
          partialMethodOrigins_(partialMethodOrigins) {}

    void run() {
        bool anyGenericSyntax = false;
        for (const auto& u : units_) {
            if (u && u->hasGenericSyntax) {
                anyGenericSyntax = true;
                break;
            }
        }
        if (!anyGenericSyntax) return;
        collectTemplates();
        validateTemplateDeclarationConstraints();
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
                                declaration.typeParameters.size(),
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
                                declaration.typeParameters.size(),
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
        declaration.rawConstraints = constraints;
        auto& table = registry_.types[moduleName];
        if (!table.emplace(nativeGenericTypeKey(name, declaration.parameters.size()),
                std::move(declaration)).second) {
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
        declaration.sourceUnit = unit;
        for (const auto& parameter : function.typeParameters) {
            declaration.parameters.push_back(parameter.text);
        }
        for (const auto& parameter : function.parameters) {
            declaration.parameterTypes.push_back(
                nativeGenericTypeText(parameter.type));
        }
        declaration.constraints = nativeGenericConstraints(function.constraints);
        declaration.rawConstraints = function.constraints;
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
        std::size_t ownerArity,
        const syntax::FunctionDeclarationSyntax& function,
        const ParsedUnit* unit) {
        NativeGenericTemplate declaration;
        declaration.kind = NativeGenericTemplate::Kind::Function;
        declaration.moduleName = moduleName;
        declaration.ownerName = ownerName;
        declaration.ownerArity = ownerArity;
        declaration.name = function.identifierToken.text;
        declaration.sourceName = unit->source->name();
        declaration.span = function.span();
        declaration.unit = unit;
        declaration.sourceUnit = unit;
        if (partialMethodOrigins_) {
            const auto key = std::make_tuple(
                moduleName,
                ownerName,
                function.identifierToken.text,
                declaration.span.start,
                declaration.span.length);
            const auto it = partialMethodOrigins_->find(key);
            if (it != partialMethodOrigins_->end() && it->second && it->second->source) {
                declaration.sourceUnit = it->second;
                declaration.sourceName = it->second->source->name();
            }
        }
        for (const auto& parameter : function.typeParameters) {
            declaration.parameters.push_back(parameter.text);
        }
        for (const auto& parameter : function.parameters) {
            declaration.parameterTypes.push_back(
                nativeGenericTypeText(parameter.type));
        }
        declaration.constraints = nativeGenericConstraints(function.constraints);
        declaration.rawConstraints = function.constraints;
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

    bool hasAnyTypeTemplateNamed(
        const std::string& moduleName,
        const std::string& name) const {
        const auto checkModule = [&](const std::string& modName) -> bool {
            const auto it = registry_.types.find(modName);
            if (it == registry_.types.end()) return false;
            for (const auto& [key, entry] : it->second) {
                (void)key;
                if (entry.name == name) return true;
            }
            return false;
        };
        if (checkModule(moduleName)) return true;
        const auto module = modules_.find(moduleName);
        if (module != modules_.end()) {
            for (const auto& importedName : module->second.imports) {
                if (checkModule(importedName)) return true;
            }
        }
        return false;
    }

    const NativeGenericTemplate* resolveType(
        const std::string& moduleName,
        const std::string& name,
        std::size_t arity,
        text::TextSpan span,
        const std::string& sourceName,
        bool* outAmbiguous = nullptr) {
        const auto key = nativeGenericTypeKey(name, arity);
        const auto own = registry_.types.find(moduleName);
        if (own != registry_.types.end()) {
            const auto found = own->second.find(key);
            if (found != own->second.end()) return &found->second;
        }
        std::vector<const NativeGenericTemplate*> candidates;
        const auto module = modules_.find(moduleName);
        if (module != modules_.end()) {
            for (const auto& importedName : module->second.imports) {
                const auto imported = registry_.types.find(importedName);
                if (imported == registry_.types.end()) continue;
                const auto found = imported->second.find(key);
                if (found != imported->second.end()) {
                    candidates.push_back(&found->second);
                }
            }
        }
        if (candidates.size() == 1) return candidates.front();
        if (candidates.size() > 1) {
            if (outAmbiguous) *outAmbiguous = true;
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
        case syntax::SyntaxKind::ThisExpression:
            return currentTypeName_;
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
        case syntax::SyntaxKind::MemberAccessExpression: {
            const auto& ma = static_cast<const
                syntax::MemberAccessExpressionSyntax&>(expression);
            const auto recvType = expressionType(*ma.receiver);
            if (!recvType.empty() && recvType == currentTypeName_) {
                const auto found = memberTypes_.find(ma.nameToken.text);
                if (found != memberTypes_.end()) return found->second;
            }
            return {};
        }
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
        const NativeGenericTemplate* declaration = nullptr;
        if (function) {
            declaration = resolve(
                consumerModule, name, true, span, sourceName);
            if (!declaration) {
                result_.diagnostics.report(
                    "RS8500",
                    "unknown generic declaration '" + name + "'",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return name;
            }
            if (arguments.size() != declaration->parameters.size()) {
                result_.diagnostics.report(
                    "RS8501",
                    "generic arity mismatch for '" + name + "'",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return name;
            }
        } else {
            bool ambiguousType = false;
            declaration = resolveType(
                consumerModule, name, arguments.size(), span, sourceName, &ambiguousType);
            if (ambiguousType) {
                return name;
            }
            if (!declaration) {
                if (builtinArity == 0) {
                    if (hasAnyTypeTemplateNamed(consumerModule, name)) {
                        result_.diagnostics.report(
                            "RS8501",
                            "generic arity mismatch for '" + name + "'",
                            span,
                            diagnostics::DiagnosticSeverity::Error,
                            sourceName);
                    } else {
                        result_.diagnostics.report(
                            "RS8500",
                            "unknown generic declaration '" + name + "'",
                            span,
                            diagnostics::DiagnosticSeverity::Error,
                            sourceName);
                    }
                    modules_[consumerModule].invalid = true;
                    return name;
                }
                if (arguments.size() != builtinArity) {
                    result_.diagnostics.report(
                        "RS8501",
                        "generic arity mismatch for '" + name + "'",
                        span,
                        diagnostics::DiagnosticSeverity::Error,
                        sourceName);
                    modules_[consumerModule].invalid = true;
                    return name;
                }
            }
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
        unit->source = std::make_shared<text::SourceText>(
            generated.str(), path);
        diagnostics::DiagnosticBag parseDiagnostics;
        syntax::Parser parser(*unit->source, parseDiagnostics);
        unit->syntaxTree = std::make_shared<syntax::CompilationUnitSyntax>(
            parser.parseCompilationUnit());
        unit->hasGenericSyntax =
            (generated.str().find('<') != std::string::npos);
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

    enum class ConstraintTypeKind { Invalid, Interface, Class, NonInheritable };

    static bool isConstraintPrimitiveOrSpecialName(const std::string& name) {
        static const std::set<std::string> names = {
            "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
            "long", "ulong", "float", "double", "char", "fixed32",
            "fixed64", "void", "string", "object"};
        return names.find(name) != names.end();
    }

    static bool isConstraintNullable(
        const std::string& rawText,
        const syntax::TypeSyntax& type) {
        if (!rawText.empty() && rawText.back() == '?') return true;
        if (rawText.rfind("Nullable__", 0) == 0 ||
            rawText.rfind("Nullable<", 0) == 0) {
            return true;
        }
        return type.name.text == "Nullable" && type.isGeneric();
    }

    template <typename Fn>
    void forEachVisibleUnit(const std::string& moduleName, Fn&& fn) const {
        const auto visitModule = [&](const std::string& modName) {
            const auto modIt = modules_.find(modName);
            if (modIt == modules_.end()) return;
            for (const auto* unitPtr : modIt->second.units) {
                if (unitPtr && unitPtr->syntaxTree) fn(modName, *unitPtr);
            }
        };
        visitModule(moduleName);
        const auto selfIt = modules_.find(moduleName);
        if (selfIt != modules_.end()) {
            for (const auto& imported : selfIt->second.imports) {
                visitModule(imported);
            }
        }
    }

    const NativeGenericTemplate* findTypeTemplateInScope(
        const std::string& moduleName,
        const std::string& typeName,
        std::size_t arity) const {
        const auto key = nativeGenericTypeKey(typeName, arity);
        const auto ownTable = registry_.types.find(moduleName);
        if (ownTable != registry_.types.end()) {
            const auto hit = ownTable->second.find(key);
            if (hit != ownTable->second.end()) return &hit->second;
        }
        const auto selfIt = modules_.find(moduleName);
        if (selfIt != modules_.end()) {
            for (const auto& imported : selfIt->second.imports) {
                const auto impTable = registry_.types.find(imported);
                if (impTable == registry_.types.end()) continue;
                const auto hit = impTable->second.find(key);
                if (hit != impTable->second.end()) return &hit->second;
            }
        }
        return nullptr;
    }

    bool validateNestedConstraintTypeArg(
        const syntax::TypeSyntax& type,
        const std::string& moduleName,
        const std::set<std::string>& ownParams,
        const std::set<std::string>& enclosingParams) const {
        if (!type.isGeneric()) {
            const auto& name = type.name.text;
            if (ownParams.find(name) != ownParams.end() ||
                enclosingParams.find(name) != enclosingParams.end() ||
                isConstraintPrimitiveOrSpecialName(name)) {
                return true;
            }
            bool found = false;
            forEachVisibleUnit(moduleName, [&](const std::string&, const ParsedUnit& u) {
                if (found) return;
                for (const auto& c : u.syntaxTree->classes) {
                    if (c.typeParameters.empty() && c.identifierToken.text == name) { found = true; return; }
                }
                for (const auto& s : u.syntaxTree->structs) {
                    if (s.typeParameters.empty() && s.identifierToken.text == name) { found = true; return; }
                }
                for (const auto& i : u.syntaxTree->interfaces) {
                    if (i.typeParameters.empty() && i.identifierToken.text == name) { found = true; return; }
                }
                for (const auto& e : u.syntaxTree->enums) {
                    if (e.identifierToken.text == name) { found = true; return; }
                }
                for (const auto& d : u.syntaxTree->delegates) {
                    if (d.typeParameters.empty() && d.identifierToken.text == name) { found = true; return; }
                }
            });
            return found;
        }

        const auto* tmpl = findTypeTemplateInScope(
            moduleName, type.name.text, type.typeArguments.size());
        const auto builtinArity = nativeBuiltinGenericArity(type.name.text);
        const std::size_t expectedArity = tmpl ? tmpl->parameters.size() : builtinArity;
        if (expectedArity == 0 || type.typeArguments.size() != expectedArity) {
            return false;
        }
        for (const auto& arg : type.typeArguments) {
            if (!validateNestedConstraintTypeArg(arg, moduleName, ownParams, enclosingParams)) {
                return false;
            }
        }
        return true;
    }

    ConstraintTypeKind classifyTopLevelConstraintType(
        const std::string& rawText,
        const syntax::TypeSyntax& type,
        const std::string& moduleName,
        const std::set<std::string>& ownParams,
        const std::set<std::string>& enclosingParams) const {
        if (type.isArray() || isConstraintNullable(rawText, type)) {
            return ConstraintTypeKind::NonInheritable;
        }
        const auto& name = type.name.text;
        if (isConstraintPrimitiveOrSpecialName(name)) {
            return ConstraintTypeKind::NonInheritable;
        }
        if (type.isGeneric()) {
            for (const auto& arg : type.typeArguments) {
                if (!validateNestedConstraintTypeArg(arg, moduleName, ownParams, enclosingParams)) {
                    return ConstraintTypeKind::Invalid;
                }
            }
            if (const auto* tmpl = findTypeTemplateInScope(
                    moduleName, name, type.typeArguments.size())) {
                if (type.typeArguments.size() != tmpl->parameters.size()) {
                    return ConstraintTypeKind::Invalid;
                }
                if (tmpl->kind == NativeGenericTemplate::Kind::Interface) {
                    return ConstraintTypeKind::Interface;
                }
                if (tmpl->kind == NativeGenericTemplate::Kind::Class) {
                    bool isSealed = false;
                    if (tmpl->unit && tmpl->unit->syntaxTree) {
                        for (const auto& cls : tmpl->unit->syntaxTree->classes) {
                            if (cls.identifierToken.text == name &&
                                cls.typeParameters.size() == tmpl->parameters.size() &&
                                semantic::hasModifier(cls.modifiers, syntax::SyntaxKind::SealedKeyword)) {
                                isSealed = true;
                                break;
                            }
                        }
                    }
                    return isSealed ? ConstraintTypeKind::NonInheritable : ConstraintTypeKind::Class;
                }
                return ConstraintTypeKind::NonInheritable;
            }
            const auto builtinArity = nativeBuiltinGenericArity(name);
            if (builtinArity > 0 && type.typeArguments.size() == builtinArity) {
                return ConstraintTypeKind::Class;
            }
            return ConstraintTypeKind::Invalid;
        }

        ConstraintTypeKind resultKind = ConstraintTypeKind::Invalid;
        forEachVisibleUnit(moduleName, [&](const std::string&, const ParsedUnit& u) {
            if (resultKind != ConstraintTypeKind::Invalid) return;
            for (const auto& iface : u.syntaxTree->interfaces) {
                if (iface.typeParameters.empty() && iface.identifierToken.text == name) {
                    resultKind = ConstraintTypeKind::Interface;
                    return;
                }
            }
            for (const auto& cls : u.syntaxTree->classes) {
                if (cls.typeParameters.empty() && cls.identifierToken.text == name) {
                    resultKind = semantic::hasModifier(cls.modifiers, syntax::SyntaxKind::SealedKeyword)
                        ? ConstraintTypeKind::NonInheritable
                        : ConstraintTypeKind::Class;
                    return;
                }
            }
            for (const auto& st : u.syntaxTree->structs) {
                if (st.typeParameters.empty() && st.identifierToken.text == name) {
                    resultKind = ConstraintTypeKind::NonInheritable;
                    return;
                }
            }
            for (const auto& en : u.syntaxTree->enums) {
                if (en.identifierToken.text == name) {
                    resultKind = ConstraintTypeKind::NonInheritable;
                    return;
                }
            }
            for (const auto& del : u.syntaxTree->delegates) {
                if (del.typeParameters.empty() && del.identifierToken.text == name) {
                    resultKind = ConstraintTypeKind::NonInheritable;
                    return;
                }
            }
        });
        return resultKind;
    }

    void validateSingleTemplateConstraints(const NativeGenericTemplate& decl) {
        if (decl.rawConstraints.empty()) return;
        std::set<std::string> ownParams(decl.parameters.begin(), decl.parameters.end());
        std::set<std::string> enclosingParams;
        if (!decl.ownerName.empty()) {
            const auto ownTable = registry_.types.find(decl.moduleName);
            if (ownTable != registry_.types.end()) {
                const auto ownerIt = ownTable->second.find(
                    nativeGenericTypeKey(decl.ownerName, decl.ownerArity));
                if (ownerIt != ownTable->second.end()) {
                    enclosingParams.insert(
                        ownerIt->second.parameters.begin(),
                        ownerIt->second.parameters.end());
                }
            }
        }

        struct ConstraintItem {
            std::string text;
            text::TextSpan span;
        };
        struct ParamState {
            bool hasClass = false;
            bool hasStruct = false;
            bool hasNew = false;
            std::vector<std::string> baseClasses;
            std::set<std::string> seenItems;
        };
        std::map<std::string, ParamState> paramStates;
        std::map<std::string, std::set<std::string>> nakedEdges;

        const auto reportDeclError = [&](const std::string& code,
                                         const std::string& msg,
                                         text::TextSpan diagSpan) {
            result_.diagnostics.report(
                code, msg, diagSpan,
                diagnostics::DiagnosticSeverity::Error,
                decl.sourceName);
            modules_[decl.moduleName].invalid = true;
        };

        for (const auto& clause : decl.rawConstraints) {
            const auto& paramName = clause.typeParameter.text;
            if (ownParams.find(paramName) == ownParams.end()) {
                reportDeclError(
                    "RS8531",
                    "unknown generic type parameter '" + paramName +
                        "' in where constraint on '" + decl.name + "'",
                    clause.typeParameter.span);
                continue;
            }

            std::vector<ConstraintItem> items;
            std::string currentText;
            text::TextSpan currentSpan{};
            bool hasSpan = false;
            int angleDepth = 0;
            const auto flushItem = [&]() {
                if (!currentText.empty()) {
                    items.push_back(ConstraintItem{currentText, currentSpan});
                }
                currentText.clear();
                hasSpan = false;
            };
            for (const auto& tok : clause.constraints) {
                if (tok.kind == syntax::SyntaxKind::LessToken) ++angleDepth;
                if (tok.kind == syntax::SyntaxKind::GreaterToken) --angleDepth;
                if (tok.kind == syntax::SyntaxKind::CommaToken && angleDepth == 0) {
                    flushItem();
                    continue;
                }
                currentText += tok.text;
                if (!hasSpan) {
                    currentSpan = tok.span;
                    hasSpan = true;
                } else if (tok.span.start >= currentSpan.start) {
                    currentSpan.length =
                        (tok.span.start + tok.span.length) - currentSpan.start;
                }
            }
            flushItem();

            auto& state = paramStates[paramName];
            for (std::size_t itemIdx = 0; itemIdx < items.size(); ++itemIdx) {
                const auto& item = items[itemIdx];
                if (!state.seenItems.insert(item.text).second) {
                    reportDeclError(
                        "RS8532",
                        "duplicate generic constraint '" + item.text +
                            "' for type parameter '" + paramName + "'",
                        item.span);
                    continue;
                }
                if (item.text == "class") {
                    if (itemIdx != 0) {
                        reportDeclError(
                            "RS8533",
                            "the 'class' constraint must come first in where clause for '" +
                                paramName + "'",
                            item.span);
                    }
                    if (state.hasStruct) {
                        reportDeclError(
                            "RS8533",
                            "type parameter '" + paramName +
                                "' cannot combine 'class' and 'struct' constraints",
                            item.span);
                    }
                    if (!state.baseClasses.empty()) {
                        reportDeclError(
                            "RS8533",
                            "type parameter '" + paramName +
                                "' cannot combine 'class' constraint with a base class constraint",
                            item.span);
                    }
                    state.hasClass = true;
                    continue;
                }
                if (item.text == "struct") {
                    if (itemIdx != 0) {
                        reportDeclError(
                            "RS8533",
                            "the 'struct' constraint must come first in where clause for '" +
                                paramName + "'",
                            item.span);
                    }
                    if (state.hasClass) {
                        reportDeclError(
                            "RS8533",
                            "type parameter '" + paramName +
                                "' cannot combine 'class' and 'struct' constraints",
                            item.span);
                    }
                    if (state.hasNew) {
                        reportDeclError(
                            "RS8533",
                            "type parameter '" + paramName +
                                "' cannot combine 'struct' and 'new()' constraints",
                            item.span);
                    }
                    if (!state.baseClasses.empty()) {
                        reportDeclError(
                            "RS8533",
                            "type parameter '" + paramName +
                                "' cannot combine 'struct' constraint with a base class constraint",
                            item.span);
                    }
                    state.hasStruct = true;
                    continue;
                }
                if (item.text == "new()") {
                    if (itemIdx + 1 != items.size()) {
                        reportDeclError(
                            "RS8533",
                            "the 'new()' constraint must be the last constraint for '" +
                                paramName + "'",
                            item.span);
                    }
                    if (state.hasStruct) {
                        reportDeclError(
                            "RS8533",
                            "type parameter '" + paramName +
                                "' cannot combine 'struct' and 'new()' constraints",
                            item.span);
                    }
                    state.hasNew = true;
                    continue;
                }
                if (ownParams.find(item.text) != ownParams.end() ||
                    enclosingParams.find(item.text) != enclosingParams.end()) {
                    if (item.text == paramName) {
                        reportDeclError(
                            "RS8533",
                            "circular constraint dependency involving type parameter '" +
                                paramName + "'",
                            item.span);
                    } else if (ownParams.find(item.text) != ownParams.end()) {
                        nakedEdges[paramName].insert(item.text);
                    }
                    continue;
                }

                text::SourceText holderSource(
                    "class __RsConstraintHolder { " + item.text + " f; }",
                    decl.sourceName);
                diagnostics::DiagnosticBag holderDiags;
                syntax::Parser holderParser(holderSource, holderDiags);
                const auto holderUnit = holderParser.parseCompilationUnit();
                if (holderDiags.hasErrors() ||
                    holderUnit.classes.empty() ||
                    holderUnit.classes.front().fields.empty()) {
                    reportDeclError(
                        "RS8533",
                        "invalid constraint '" + item.text + "' for type parameter '" +
                            paramName + "'",
                        item.span);
                    continue;
                }
                const auto& parsedType = holderUnit.classes.front().fields.front().type;
                const auto ctKind = classifyTopLevelConstraintType(
                    item.text, parsedType, decl.moduleName, ownParams, enclosingParams);
                if (ctKind == ConstraintTypeKind::Invalid) {
                    reportDeclError(
                        "RS8533",
                        "unknown or malformed constraint type '" + item.text +
                            "' for type parameter '" + paramName + "'",
                        item.span);
                } else if (ctKind == ConstraintTypeKind::NonInheritable) {
                    reportDeclError(
                        "RS8533",
                        "constraint '" + item.text + "' for type parameter '" +
                            paramName + "' is not a valid class or interface constraint",
                        item.span);
                } else if (ctKind == ConstraintTypeKind::Class) {
                    if (state.hasClass || state.hasStruct) {
                        reportDeclError(
                            "RS8533",
                            "type parameter '" + paramName +
                                "' cannot combine base class constraint '" + item.text +
                                "' with 'class' or 'struct'",
                            item.span);
                    }
                    if (!state.baseClasses.empty()) {
                        reportDeclError(
                            "RS8533",
                            "type parameter '" + paramName +
                                "' cannot specify multiple base class constraints ('" +
                                item.text + "')",
                            item.span);
                    }
                    state.baseClasses.push_back(item.text);
                }
            }
        }

        // 3-color DFS cycle check on naked type parameter constraints among decl.parameters
        std::map<std::string, int> color; // 0=unvisited, 1=visiting, 2=done
        std::function<bool(const std::string&)> hasCycle = [&](const std::string& node) -> bool {
            color[node] = 1;
            const auto edgeIt = nakedEdges.find(node);
            if (edgeIt != nakedEdges.end()) {
                for (const auto& next : edgeIt->second) {
                    const int nextColor = color[next];
                    if (nextColor == 1) return true;
                    if (nextColor == 0 && hasCycle(next)) return true;
                }
            }
            color[node] = 2;
            return false;
        };
        for (const auto& param : decl.parameters) {
            if (color[param] == 0 && hasCycle(param)) {
                reportDeclError(
                    "RS8533",
                    "circular generic type parameter constraint on '" + decl.name + "'",
                    decl.span);
                break;
            }
        }
    }

    void validateTemplateDeclarationConstraints() {
        for (const auto& [modName, table] : registry_.types) {
            (void)modName;
            for (const auto& [declName, decl] : table) {
                (void)declName;
                validateSingleTemplateConstraints(decl);
            }
        }
        for (const auto& [modName, table] : registry_.functions) {
            (void)modName;
            for (const auto& [declName, decl] : table) {
                (void)declName;
                validateSingleTemplateConstraints(decl);
            }
        }
        for (const auto& [modName, table] : registry_.members) {
            (void)modName;
            for (const auto& [memberName, declList] : table) {
                (void)memberName;
                for (const auto& decl : declList) {
                    validateSingleTemplateConstraints(decl);
                }
            }
        }
    }

    static std::string substituteTypeText(
        const syntax::TypeSyntax& type,
        const std::map<std::string, std::string>* subs) {
        if (!type.isGeneric()) {
            std::string base = type.name.text;
            if (subs) {
                const auto it = subs->find(base);
                if (it != subs->end()) base = it->second;
            }
            if (type.isArray()) base += "[]";
            return base;
        }
        std::string result = type.name.text + "<";
        for (std::size_t i = 0; i < type.typeArguments.size(); ++i) {
            if (i != 0) result.push_back(',');
            result += substituteTypeText(type.typeArguments[i], subs);
        }
        result.push_back('>');
        if (type.isArray()) result += "[]";
        return result;
    }

    static std::string canonicalGenericTypeName(
        const syntax::TypeSyntax& type,
        const std::map<std::string, std::string>* subs = nullptr) {
        if (!type.isGeneric()) {
            std::string base = type.name.text;
            if (subs) {
                const auto it = subs->find(base);
                if (it != subs->end()) base = it->second;
            }
            if (type.isArray()) base += "[]";
            return base;
        }
        std::vector<std::string> args;
        args.reserve(type.typeArguments.size());
        for (const auto& arg : type.typeArguments) {
            args.push_back(canonicalGenericTypeName(arg, subs));
        }
        auto result = nativeGenericGeneratedName(type.name.text, args);
        if (type.isArray()) result += "[]";
        return result;
    }

    void collectTemplateSupertypes(
        const std::string& templateName,
        const std::vector<std::string>& args,
        std::set<std::string>& outSupertypes,
        std::set<std::string>& visited) const {
        for (const auto& u : units_) {
            if (!u || !u->syntaxTree) continue;
            const auto handleBases = [&](const std::vector<syntax::SyntaxToken>& typeParams,
                                         const std::vector<syntax::TypeSyntax>& bases) {
                if (typeParams.size() != args.size() || typeParams.empty()) return;
                std::map<std::string, std::string> subs;
                for (std::size_t i = 0; i < typeParams.size(); ++i) {
                    subs.emplace(typeParams[i].text, args[i]);
                }
                for (const auto& base : bases) {
                    const auto rawText = substituteTypeText(base, &subs);
                    outSupertypes.insert(rawText);
                    collectSupertypes(rawText, outSupertypes, visited);
                    if (base.isGeneric()) {
                        const auto canon = canonicalGenericTypeName(base, &subs);
                        outSupertypes.insert(canon);
                        collectSupertypes(canon, outSupertypes, visited);
                        std::vector<std::string> nextArgs;
                        nextArgs.reserve(base.typeArguments.size());
                        for (const auto& a : base.typeArguments) {
                            nextArgs.push_back(canonicalGenericTypeName(a, &subs));
                        }
                        collectTemplateSupertypes(base.name.text, nextArgs, outSupertypes, visited);
                    }
                }
            };
            for (const auto& cls : u->syntaxTree->classes) {
                if (cls.identifierToken.text == templateName) {
                    handleBases(cls.typeParameters, cls.interfaces);
                }
            }
            for (const auto& st : u->syntaxTree->structs) {
                if (st.identifierToken.text == templateName) {
                    handleBases(st.typeParameters, st.interfaces);
                }
            }
            for (const auto& iface : u->syntaxTree->interfaces) {
                if (iface.identifierToken.text == templateName) {
                    handleBases(iface.typeParameters, iface.interfaces);
                }
            }
        }
    }

    struct ConcreteTypeFacts {
        bool known = false;
        bool valueType = false;
        bool referenceType = false;
        bool defaultConstructor = false;
        std::set<std::string> supertypes;
    };

    void collectSupertypes(
        const std::string& typeName,
        std::set<std::string>& outSupertypes,
        std::set<std::string>& visited) const {
        if (typeName.empty() || !visited.insert(typeName).second) return;
        outSupertypes.insert(typeName);
        const auto recordInterfaces = [&](const std::vector<syntax::TypeSyntax>& ifaces) {
            for (const auto& iface : ifaces) {
                const auto parentName = nativeGenericTypeText(iface);
                outSupertypes.insert(parentName);
                collectSupertypes(parentName, outSupertypes, visited);
                if (iface.isGeneric()) {
                    const auto canon = canonicalGenericTypeName(iface);
                    outSupertypes.insert(canon);
                    collectSupertypes(canon, outSupertypes, visited);
                    std::vector<std::string> subArgs;
                    subArgs.reserve(iface.typeArguments.size());
                    for (const auto& arg : iface.typeArguments) {
                        subArgs.push_back(canonicalGenericTypeName(arg));
                    }
                    collectTemplateSupertypes(iface.name.text, subArgs, outSupertypes, visited);
                }
            }
        };
        for (const auto& unit : units_) {
            for (const auto& value : unit->syntaxTree->classes) {
                if (!value.typeParameters.empty()) continue;
                if (value.identifierToken.text != typeName) continue;
                recordInterfaces(value.interfaces);
            }
            for (const auto& value : unit->syntaxTree->structs) {
                if (!value.typeParameters.empty()) continue;
                if (value.identifierToken.text != typeName) continue;
                recordInterfaces(value.interfaces);
            }
            for (const auto& value : unit->syntaxTree->interfaces) {
                if (!value.typeParameters.empty()) continue;
                if (value.identifierToken.text != typeName) continue;
                recordInterfaces(value.interfaces);
            }
        }
    }

    ConcreteTypeFacts concreteTypeFacts(const std::string& name) const {
        static const std::set<std::string> primitiveValues = {
            "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
            "long", "ulong", "float", "double", "char", "fixed32",
            "fixed64"};
        if (primitiveValues.find(name) != primitiveValues.end()) {
            return {true, true, false, true, {name, "object"}};
        }
        if ((name.size() > 1 && name.back() == '?') ||
            name.rfind("Nullable__", 0) == 0 ||
            name.rfind("Nullable<", 0) == 0) {
            return {true, false, false, true, {name, "object"}};
        }
        if (name == "string" || name == "object" ||
            (name.size() > 2 && name.substr(name.size() - 2) == "[]")) {
            return {true, false, true, false, {name, "object"}};
        }
        std::set<std::string> supertypes;
        std::set<std::string> visited;
        collectSupertypes(name, supertypes, visited);
        supertypes.insert("object");
        for (const auto& unit : units_) {
            for (const auto& value : unit->syntaxTree->structs) {
                if (!value.typeParameters.empty()) continue;
                if (value.identifierToken.text == name) {
                    return {true, true, false, true, std::move(supertypes)};
                }
            }
            for (const auto& value : unit->syntaxTree->enums) {
                if (value.identifierToken.text == name) {
                    return {true, true, false, true, std::move(supertypes)};
                }
            }
            for (const auto& value : unit->syntaxTree->classes) {
                if (!value.typeParameters.empty()) continue;
                if (value.identifierToken.text != name) continue;
                bool hasInstanceConstructor = false;
                bool defaultConstructor = false;
                for (const auto& constructor : value.constructors) {
                    if (semantic::hasModifier(
                            constructor.modifiers,
                            syntax::SyntaxKind::StaticKeyword)) {
                        continue;
                    }
                    hasInstanceConstructor = true;
                    const bool accessible =
                        !semantic::hasModifier(
                            constructor.modifiers,
                            syntax::SyntaxKind::PrivateKeyword) &&
                        !semantic::hasModifier(
                            constructor.modifiers,
                            syntax::SyntaxKind::ProtectedKeyword);
                    if (accessible && constructor.parameters.empty()) {
                        defaultConstructor = true;
                    }
                }
                if (!hasInstanceConstructor) defaultConstructor = true;
                if (semantic::hasModifier(
                        value.modifiers,
                        syntax::SyntaxKind::AbstractKeyword)) {
                    defaultConstructor = false;
                }
                return {true, false, true, defaultConstructor, std::move(supertypes)};
            }
            for (const auto& value : unit->syntaxTree->interfaces) {
                if (!value.typeParameters.empty()) continue;
                if (value.identifierToken.text == name) {
                    return {true, false, true, false, std::move(supertypes)};
                }
            }
            for (const auto& value : unit->syntaxTree->delegates) {
                if (!value.typeParameters.empty()) continue;
                if (value.identifierToken.text == name) {
                    return {true, false, true, false, std::move(supertypes)};
                }
            }
        }
        return {};
    }

    std::string resolveConstraintTypeName(
        const std::string& rawConstraint,
        const NativeGenericTemplate& declaration,
        const std::vector<std::string>& arguments,
        const std::string& consumerModule,
        text::TextSpan span,
        const std::string& sourceName,
        std::string* outSubstitutedText = nullptr) {
        std::map<std::string, std::string> substitutions;
        for (std::size_t i = 0; i < declaration.parameters.size() && i < arguments.size(); ++i) {
            substitutions.emplace(declaration.parameters[i], arguments[i]);
        }
        (void)span;
        text::SourceText source("class __RsConstraintHolder { " + rawConstraint + " f; }", sourceName);
        diagnostics::DiagnosticBag ignored;
        syntax::Parser parser(source, ignored);
        auto unit = parser.parseCompilationUnit();
        if (unit.classes.empty() || unit.classes.front().fields.empty()) {
            if (outSubstitutedText) *outSubstitutedText = rawConstraint;
            return rawConstraint;
        }
        auto parsedType = unit.classes.front().fields.front().type;
        std::function<void(syntax::TypeSyntax&)> substituteType = [&](syntax::TypeSyntax& t) {
            const auto sub = substitutions.find(t.name.text);
            if (sub != substitutions.end() && !t.isGeneric()) {
                t.name.text = sub->second;
            }
            for (auto& arg : t.typeArguments) {
                substituteType(arg);
            }
        };
        substituteType(parsedType);
        if (outSubstitutedText) {
            *outSubstitutedText = nativeGenericTypeText(parsedType);
        }
        if (parsedType.isGeneric()) {
            const auto savedLocalTypes = localTypes_;
            std::vector<ParsedUnit*> dummyQueue;
            rewriteType(parsedType, consumerModule, sourceName, dummyQueue);
            for (std::size_t qIdx = 0; qIdx < dummyQueue.size(); ++qIdx) {
                auto* u = dummyQueue[qIdx];
                if (!u || !processed_.insert(u).second) continue;
                rewriteUnit(*u, dummyQueue);
            }
            localTypes_ = savedLocalTypes;
        }
        return nativeGenericTypeText(parsedType);
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
                std::size_t nakedIdx = declaration.parameters.size();
                for (std::size_t j = 0; j < declaration.parameters.size(); ++j) {
                    if (declaration.parameters[j] == constraint) {
                        nakedIdx = j;
                        break;
                    }
                }
                if (constraint == "class") {
                    satisfied = facts.referenceType;
                } else if (constraint == "struct") {
                    satisfied = facts.valueType;
                } else if (constraint == "new()") {
                    satisfied = facts.defaultConstructor;
                } else if (nakedIdx < arguments.size()) {
                    const auto& targetArg = arguments[nakedIdx];
                    satisfied = (arguments[index] == targetArg) ||
                        (facts.known &&
                         (targetArg == "object" ||
                          facts.supertypes.find(targetArg) != facts.supertypes.end()));
                } else {
                    std::string substitutedType;
                    const auto expectedType = resolveConstraintTypeName(
                        constraint, declaration, arguments, consumerModule, span, sourceName, &substitutedType);
                    satisfied = facts.known &&
                        (facts.supertypes.find(expectedType) != facts.supertypes.end() ||
                         (!substitutedType.empty() &&
                          facts.supertypes.find(substitutedType) != facts.supertypes.end()) ||
                         facts.supertypes.find(constraint) != facts.supertypes.end());
                }
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
        detachSyntaxTree(*ownerUnit);
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
        case syntax::SyntaxKind::LambdaExpression: {
            auto& lam = static_cast<syntax::LambdaExpressionSyntax&>(expression);
            const auto prevLocals = localTypes_;
            for (std::size_t idx = 0; idx < lam.parameterTypes.size(); ++idx) {
                if (!lam.parameterTypes[idx].has_value()) continue;
                rewriteType(*lam.parameterTypes[idx], moduleName, sourceName, queue);
                if (idx < lam.parameterTokens.size() &&
                    lam.parameterTokens[idx].text != "_") {
                    localTypes_[lam.parameterTokens[idx].text] =
                        typeText(*lam.parameterTypes[idx]);
                }
            }
            if (lam.blockBody) {
                rewriteStatement(*lam.blockBody, moduleName, sourceName, queue);
            } else if (lam.body) {
                rewriteExpression(*lam.body, moduleName, sourceName, queue);
            }
            localTypes_ = prevLocals;
            return;
        }
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
            const NativeGenericTemplate* memberDecl = !currentTypeName_.empty()
                ? resolveMember(
                      moduleName,
                      call.identifierToken.text,
                      currentTypeName_,
                      call.span(),
                      sourceName)
                : nullptr;
            if (call.lessToken) {
                for (auto& argument : call.typeArguments) {
                    rewriteType(argument, moduleName, sourceName, queue);
                }
                std::vector<std::string> arguments;
                for (const auto& argument : call.typeArguments) {
                    arguments.push_back(typeText(argument));
                }
                if (memberDecl) {
                    call.identifierToken.text = requestMember(
                        moduleName,
                        *memberDecl,
                        arguments,
                        call.span(),
                        sourceName);
                } else {
                    call.identifierToken.text = request(
                        moduleName,
                        call.identifierToken.text,
                        std::move(arguments),
                        true,
                        call.span(),
                        sourceName,
                        queue);
                }
                call.lessToken.reset();
                call.typeArguments.clear();
                call.typeArgumentCommaTokens.clear();
                call.greaterToken.reset();
            } else if (memberDecl) {
                const auto inferred = inferArguments(*memberDecl, call.arguments);
                if (inferred) {
                    call.identifierToken.text = requestMember(
                        moduleName,
                        *memberDecl,
                        *inferred,
                        call.span(),
                        sourceName);
                }
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
            for (auto& init : value.initializers) {
                for (auto& expr : init.expressions) {
                    if (expr) rewriteExpression(*expr, moduleName, sourceName, queue);
                }
            }
            return;
        }
        case syntax::SyntaxKind::NewArrayExpression: {
            auto& value = static_cast<syntax::NewArrayExpressionSyntax&>(expression);
            rewriteType(value.elementType, moduleName, sourceName, queue);
            if (value.length) {
                rewriteExpression(*value.length, moduleName, sourceName, queue);
            }
            for (auto& item : value.initialValues) {
                if (item) rewriteExpression(*item, moduleName, sourceName, queue);
            }
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
            if (value.collection &&
                value.collection->kind() == syntax::SyntaxKind::CallExpression) {
                auto& call = static_cast<syntax::CallExpressionSyntax&>(*value.collection);
                if (call.identifierToken.text == "Query" && call.lessToken.has_value()) {
                    std::vector<syntax::TypeSyntax*> dataCompTypes;
                    for (std::size_t idx = 0; idx < call.typeArguments.size(); ++idx) {
                        auto& qArg = call.typeArguments[idx];
                        if ((qArg.name.text == "With" || qArg.name.text == "Without") &&
                            qArg.typeArguments.size() == 1) {
                            rewriteType(qArg.typeArguments[0], moduleName, sourceName, queue);
                            continue;
                        }
                        syntax::TypeSyntax* compType = &qArg;
                        if ((qArg.name.text == "Write" || qArg.name.text == "Read") &&
                            qArg.typeArguments.size() == 1) {
                            compType = &qArg.typeArguments[0];
                        }
                        rewriteType(*compType, moduleName, sourceName, queue);
                        dataCompTypes.push_back(compType);
                    }
                    const std::size_t totalBindings = 1 + value.additionalBindings.size();
                    const bool hasEntityId = (totalBindings == dataCompTypes.size() + 1 && !dataCompTypes.empty());
                    const auto getBindingType = [&](std::size_t bIdx) -> syntax::TypeSyntax* {
                        if (bIdx == 0) return &value.type;
                        if (bIdx - 1 < value.additionalBindings.size()) {
                            return &value.additionalBindings[bIdx - 1].type;
                        }
                        return nullptr;
                    };
                    const auto getBindingName = [&](std::size_t bIdx) -> std::string {
                        if (bIdx == 0) return value.identifierToken.text;
                        if (bIdx - 1 < value.additionalBindings.size()) {
                            return value.additionalBindings[bIdx - 1].identifierToken.text;
                        }
                        return {};
                    };
                    if (hasEntityId) {
                        if (auto* entType = getBindingType(0)) {
                            if (entType->name.text == "var") {
                                clearGenericType(*entType, "int");
                            } else {
                                rewriteType(*entType, moduleName, sourceName, queue);
                            }
                            const auto entName = getBindingName(0);
                            if (!entName.empty()) {
                                localTypes_[entName] = typeText(*entType);
                            }
                        }
                    }
                    for (std::size_t idx = 0; idx < dataCompTypes.size(); ++idx) {
                        const std::size_t bIdx = hasEntityId ? (idx + 1) : idx;
                        auto* bType = getBindingType(bIdx);
                        const auto bName = getBindingName(bIdx);
                        if (bType) {
                            if (bType->name.text == "var") {
                                *bType = *dataCompTypes[idx];
                            } else {
                                rewriteType(*bType, moduleName, sourceName, queue);
                            }
                            if (!bName.empty()) {
                                localTypes_[bName] = typeText(*bType);
                            }
                        }
                    }
                    rewriteStatement(*value.body, moduleName, sourceName, queue);
                    return;
                }
            }
            rewriteType(value.type, moduleName, sourceName, queue);
            for (auto& extra : value.additionalBindings) {
                rewriteType(extra.type, moduleName, sourceName, queue);
            }
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
        localTypes_ = memberTypes_;
        if (!currentTypeName_.empty()) {
            localTypes_["this"] = currentTypeName_;
        }
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
        const auto prevTypeName = currentTypeName_;
        const auto prevMemberTypes = memberTypes_;
        currentTypeName_ = declaration.identifierToken.text;
        memberTypes_.clear();
        for (auto& interfaceType : declaration.interfaces) rewriteType(interfaceType, moduleName, sourceName, queue);
        for (auto& field : declaration.fields) {
            rewriteType(field.type, moduleName, sourceName, queue);
            memberTypes_[field.identifierToken.text] = typeText(field.type);
        }
        for (auto& property : declaration.properties) {
            rewriteType(property.type, moduleName, sourceName, queue);
            memberTypes_[property.identifierToken.text] = typeText(property.type);
        }
        for (auto& field : declaration.fields) {
            if (field.initializer) rewriteExpression(*field.initializer, moduleName, sourceName, queue);
        }
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
            const auto prevLocals = localTypes_;
            localTypes_ = memberTypes_;
            localTypes_["this"] = currentTypeName_;
            for (auto& parameter : constructor.parameters) {
                rewriteParameter(parameter, moduleName, sourceName, queue);
                localTypes_[parameter.identifierToken.text] = typeText(parameter.type);
            }
            for (auto& baseArg : constructor.baseArguments) {
                if (baseArg) rewriteExpression(*baseArg, moduleName, sourceName, queue);
            }
            rewriteStatement(constructor.body, moduleName, sourceName, queue);
            localTypes_ = prevLocals;
        }
        for (auto& property : declaration.properties) {
            const auto prevLocals = localTypes_;
            localTypes_ = memberTypes_;
            localTypes_["this"] = currentTypeName_;
            localTypes_["value"] = typeText(property.type);
            if (property.getter && property.getter->body) rewriteStatement(*property.getter->body, moduleName, sourceName, queue);
            if (property.setter && property.setter->body) rewriteStatement(*property.setter->body, moduleName, sourceName, queue);
            if (property.initializer) rewriteExpression(*property.initializer, moduleName, sourceName, queue);
            localTypes_ = prevLocals;
        }
        currentTypeName_ = prevTypeName;
        memberTypes_ = prevMemberTypes;
    }

    static void detachSyntaxTree(ParsedUnit& unit) {
        if (unit.syntaxTree.use_count() > 1 && unit.source) {
            diagnostics::DiagnosticBag ignored;
            syntax::Parser parser(*unit.source, ignored);
            unit.syntaxTree = std::make_shared<syntax::CompilationUnitSyntax>(
                parser.parseCompilationUnit());
        }
    }

    void rewriteUnit(ParsedUnit& unit, std::vector<ParsedUnit*>& queue) {
        if (!unit.hasGenericSyntax && registry_.functions.empty() && registry_.members.empty()) {
            return;
        }
        detachSyntaxTree(unit);
        const auto& moduleName = unit.moduleName;
        const auto sourceName = unit.source->name();
        for (auto& declaration : unit.syntaxTree->classes) rewriteTypeDeclaration(declaration, moduleName, sourceName, queue);
        for (auto& declaration : unit.syntaxTree->structs) rewriteTypeDeclaration(declaration, moduleName, sourceName, queue);
        for (auto& declaration : unit.syntaxTree->interfaces) {
            if (!declaration.typeParameters.empty()) continue;
            for (auto& interfaceType : declaration.interfaces) {
                rewriteType(interfaceType, moduleName, sourceName, queue);
            }
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
    std::string currentTypeName_;
    std::map<std::string, std::string> memberTypes_;
    std::map<std::string, std::string> localTypes_;
    std::vector<PendingNativeGenericMember> pendingMembers_;
    const PartialMethodOriginMap* partialMethodOrigins_ = nullptr;
};

void specializeNativeGenerics(
    std::vector<std::unique_ptr<ParsedUnit>>& units,
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result,
    const PartialMethodOriginMap* partialMethodOrigins = nullptr) {
    NativeGenericSpecializer specializer(units, modules, result, partialMethodOrigins);
    specializer.run();
}
