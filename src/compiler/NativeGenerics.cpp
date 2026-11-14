#include "CompilationInternal.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::compiler::detail {

namespace {

// Native bounded generic specialization (AST-level hygienic monomorphization).
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
    std::vector<syntax::TypeSyntax> parameterTypeSyntaxes;
    std::optional<syntax::TypeSyntax> returnTypeSyntax;
    bool isExtension = false;
    std::map<std::string, std::vector<std::string>> constraints;
    const ParsedUnit* unit = nullptr;
    std::shared_ptr<syntax::ClassDeclarationSyntax> classSyntax;
    std::shared_ptr<syntax::StructDeclarationSyntax> structSyntax;
    std::shared_ptr<syntax::InterfaceDeclarationSyntax> interfaceSyntax;
    std::shared_ptr<syntax::DelegateDeclarationSyntax> delegateSyntax;
    std::shared_ptr<syntax::FunctionDeclarationSyntax> functionSyntax;
};

struct NativeGenericRegistry {
    std::map<std::string, std::map<std::string, NativeGenericTemplate>> types;
    std::map<std::string, std::map<std::string, NativeGenericTemplate>> functions;
    std::map<std::string,
        std::map<std::string, std::deque<NativeGenericTemplate>>> members;
};

struct HygienicSubstitutedType {
    std::string canonicalBaseName;
    std::string simpleBaseName;
    std::string ownerModule;
    bool isNullable = false;
    bool isArray = false;
    std::uint32_t arrayRank = 0;
};

struct PendingNativeGenericMember {
    const NativeGenericTemplate* declaration = nullptr;
    std::string consumerModule;
    std::vector<HygienicSubstitutedType> resolvedArgs;
    std::string generatedName;
    text::TextSpan requestSpan;
    std::string sourceName;
    std::uint32_t genericDepth = 0;
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

std::string stripCanonicalModulePrefix(const std::string& typeName) {
    const auto sep = typeName.find("::");
    return sep == std::string::npos ? typeName : typeName.substr(sep + 2);
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
        name == "HashSet" || name == "Awaitable" || name == "ResourceHandle") {
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
    const std::string first = "__RsBuiltinTypeArg0";
    const std::string second = "__RsBuiltinTypeArg1";
    std::ostringstream source;
    if (name == "ResourceHandle") {
        source << "public struct " << generatedName << " : IDisposable {"
            << "public " << first << " __target;"
            << "public long __leaseId;"
            << "public long __resourceId;"
            << "public " << generatedName << "(" << first << " target, long leaseId, long resourceId){"
            << "__target=target;__leaseId=leaseId;__resourceId=resourceId;}"
            << "public static " << generatedName << " Empty(){"
            << "return new " << generatedName << "(null,0L,0L);}"
            << "public static " << generatedName << " __AssignHandle(" << generatedName << " oldVal," << generatedName << " newVal){"
            << "if(oldVal.__leaseId>0L&&oldVal.__leaseId!=newVal.__leaseId){ResourceServer.__ReleaseLease(oldVal.__leaseId);}"
            << "return newVal;}"
            << "public static " << generatedName << " Adopt(" << first << " target){"
            << "if(target==null)return new " << generatedName << "(null,0L,0L);"
            << "long rid=target.ResourceId();"
            << "long lid=ResourceServer.__AllocObjectLease(target,rid,false);"
            << "return new " << generatedName << "(target,lid,rid);}"
            << "public static " << generatedName << " FromResource(" << first << " target){"
            << "if(target==null)return new " << generatedName << "(null,0L,0L);"
            << "long rid=target.ResourceId();"
            << "long lid=ResourceServer.__AllocObjectLease(target,rid,true);"
            << "return new " << generatedName << "(target,lid,rid);}"
            << "public " << generatedName << " Retain(){"
            << "if(__target==null||!IsValid())return new " << generatedName << "(null,0L,0L);"
            << "long lid=ResourceServer.__AllocObjectLease(__target,__resourceId,true);"
            << "if(lid<=0L)return new " << generatedName << "(null,0L,0L);"
            << "return new " << generatedName << "(__target,lid,__resourceId);}"
            << "public int Release(){if(__leaseId<=0L||!ResourceServer.__IsLeaseActive(__leaseId))return UseCount();return ResourceServer.__ReleaseLease(__leaseId);}"
            << "public void Dispose(){ResourceServer.__ReleaseLease(__leaseId);return;}"
            << "public int UseCount(){if(__resourceId<=0L)return 0;return ResourceServer.GetUseCount(__resourceId);}"
            << "public bool IsDisposed(){if(__resourceId<=0L)return true;return ResourceServer.IsResourceDisposed(__resourceId);}"
            << "public bool IsValid(){return __leaseId>0L&&ResourceServer.__IsLeaseActive(__leaseId)&&!IsDisposed();}"
            << "public long ResourceId(){return __resourceId;}"
            << "public string GetPath(){return ResourceServer.GetPath(__resourceId);}"
            << "public string GetGuid(){return ResourceServer.GetGuid(__resourceId);}"
            << "public string Path{get{return GetPath();}}"
            << "public string Guid{get{return GetGuid();}}"
            << "public " << first << " Get(){if(!IsValid())return null;return __target;}"
            << "public ResourceHandle AsUntyped(){"
            << "if(!IsValid())return new ResourceHandle(0L,0L);"
            << "long lid=ResourceServer.__AllocLease(__resourceId,true);"
            << "if(lid<=0L)return new ResourceHandle(0L,0L);"
            << "return new ResourceHandle(lid,__resourceId);}"
            << "}";
        return source.str();
    }
    if (name == "Awaitable") {
        source << "public struct " << generatedName << "{"
            << "public long __handle;"
            << "public " << generatedName << "(long h){__handle=h;}"
            << "public Awaitable AsUntyped(){return new Awaitable(__handle);}"
            << "public bool IsCompleted(){return Async.__IsCompleted(__handle);}"
            << "public bool Release(){return Async.__Release(__handle);}"
            << "public static " << generatedName << " FromHandle(long h){return new " << generatedName << "(h);}"
            << "public static async " << generatedName << " FromResult(" << first << " value){return value;}"
            << "public static " << generatedName << " FromCanceled(){return new " << generatedName << "(Async.__FromCanceled());}"
            << "public static " << generatedName << " FromFaulted(string msg){return new " << generatedName << "(Async.__FromFaulted(msg));}"
            << "}";
        return source.str();
    }
    if (name == "List") {
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] items;int count;" << first << " __defaultItem;"
            << generatedName << "(){items=new " << first << "[4];count=0;}"
            << generatedName << "(int capacity){int cap=capacity;if(cap<1)cap=4;items=new " << first
            << "[cap];count=0;}"
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
            << "items[i]=items[i+1];i=i+1;}count=count-1;items[count]=__defaultItem;return;}"
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
            << "void Clear(){int i=0;while(i<count){items[i]=__defaultItem;i=i+1;}count=0;return;}" << enumerator
            << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "struct " << enumerator << "{" << generatedName
            << " __owner;int __index;" << enumerator << "(" << generatedName
            << " value){__owner=value;__index=-1;}"
            << "bool MoveNext(){if(__owner==null)return false;__index=__index+1;return __index<__owner.Count();}"
            << first << " Current(){return __owner.Get(__index);}}";
    } else if (name == "Queue") {
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] items;int head;int tail;int count;" << first << " __defaultItem;"
            << generatedName << "(){items=new " << first
            << "[4];head=0;tail=0;count=0;}"
            << generatedName << "(int capacity){int cap=capacity;if(cap<1)cap=4;items=new " << first
            << "[cap];head=0;tail=0;count=0;}"
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
            << "items[head]=__defaultItem;"
            << "head=(head+1)%items.length;count=count-1;return value;}"
            << first << " Peek(){return items[head];}"
            << first << " Get(int index){return items[(head+index)%items.length];}"
            << "bool Contains(" << first << " value){int i=0;while(i<count){"
            << "if(items[(head+i)%items.length]==value)return true;i=i+1;}return false;}"
            << first << "[] ToArray(){" << first << "[] arr=new " << first
            << "[count];int i=0;while(i<count){arr[i]=items[(head+i)%items.length];i=i+1;}return arr;}"
            << "void Clear(){int i=0;while(i<count){items[(head+i)%items.length]=__defaultItem;i=i+1;}head=0;tail=0;count=0;return;}" << enumerator
            << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "struct " << enumerator << "{" << generatedName
            << " __owner;int __index;" << enumerator << "(" << generatedName
            << " value){__owner=value;__index=-1;}bool MoveNext(){if(__owner==null)return false;__index=__index+1;"
            << "return __index<__owner.Count();}" << first
            << " Current(){return __owner.Get(__index);}}";
    } else if (name == "Stack") {
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] items;int count;" << first << " __defaultItem;" << generatedName << "(){items=new "
            << first << "[4];count=0;}" << generatedName
            << "(int capacity){int cap=capacity;if(cap<1)cap=4;items=new " << first << "[cap];count=0;}"
            << "void EnsureCapacity(int needed){if(needed<=items.length)return;"
            << "int next=items.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << first << "[] grown=new " << first << "[next];int i=0;"
            << "while(i<count){grown[i]=items[i];i=i+1;}items=grown;return;}"
            << "int Count(){return count;}int Capacity(){return items.length;}"
            << "bool IsEmpty(){return count==0;}"
            << "void Push(" << first << " value){EnsureCapacity(count+1);"
            << "items[count]=value;count=count+1;return;}"
            << first << " Pop(){count=count-1;" << first << " value=items[count];items[count]=__defaultItem;return value;}"
            << first << " Peek(){return items[count-1];}"
            << first << " Get(int index){return items[count-1-index];}"
            << "bool Contains(" << first << " value){int i=0;while(i<count){"
            << "if(items[i]==value)return true;i=i+1;}return false;}"
            << first << "[] ToArray(){" << first << "[] arr=new " << first
            << "[count];int i=0;while(i<count){arr[i]=items[count-1-i];i=i+1;}return arr;}"
            << "void Clear(){int i=0;while(i<count){items[i]=__defaultItem;i=i+1;}count=0;return;}" << enumerator
            << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "struct " << enumerator << "{" << generatedName
            << " __owner;int __index;" << enumerator << "(" << generatedName
            << " value){__owner=value;__index=-1;}bool MoveNext(){if(__owner==null)return false;__index=__index+1;"
            << "return __index<__owner.Count();}" << first
            << " Current(){return __owner.Get(__index);}}";
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
            << "{bool hasValue;" << first << " value;" << first << " __defaultValue;"
            << generatedName << "(){hasValue=false;}"
            << generatedName << "(" << first
            << " initial){value=initial;hasValue=true;}"
            << "bool HasValue(){return hasValue;}"
            << first << " Value(){return value;}"
            << "void Set(" << first
            << " next){value=next;hasValue=true;return;}"
            << "void Clear(){value=__defaultValue;hasValue=false;return;}}";
    } else if (name == "HashSet") {
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] values;int[] buckets;int[] nexts;int count;" << first << " __defaultItem;" << generatedName
            << "(){values=new " << first << "[4];buckets=new int[8];nexts=new int[4];count=0;}"
            << generatedName << "(int capacity){int cap=capacity;if(cap<1)cap=4;"
            << "int bcap=8;while(bcap<cap*2){bcap=bcap*2;}"
            << "values=new " << first << "[cap];buckets=new int[bcap];nexts=new int[cap];count=0;}"
            << "int BucketFor(" << first << " value,int bcap){"
            << "int h=__rs_stable_hash(value);"
            << "if(h==-2147483647-1)h=0;else if(h<0)h=0-h;"
            << "return h%bcap;}"
            << "void Rehash(){int bcap=buckets.length;int bi=0;while(bi<bcap){buckets[bi]=0;bi=bi+1;}"
            << "int i=0;while(i<count){int b=BucketFor(values[i],bcap);"
            << "nexts[i]=buckets[b];buckets[b]=i+1;i=i+1;}return;}"
            << "void EnsureCapacity(int needed){if(needed<=values.length)return;"
            << "int next=values.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << "int bcap=8;while(bcap<next*2){bcap=bcap*2;}"
            << first << "[] grown=new " << first << "[next];int i=0;"
            << "while(i<count){grown[i]=values[i];i=i+1;}values=grown;"
            << "buckets=new int[bcap];nexts=new int[next];Rehash();return;}"
            << "int Count(){return count;}int Capacity(){return values.length;}"
            << "bool IsEmpty(){return count==0;}"
            << "int FindIndex(" << first << " value){if(count==0)return -1;"
            << "int bcap=buckets.length;if(bcap==0)return -1;"
            << "int cur=buckets[BucketFor(value,bcap)]-1;while(cur>=0){"
            << "if(values[cur]==value)return cur;cur=nexts[cur]-1;}return -1;}"
            << "bool Contains(" << first << " value){return FindIndex(value)>=0;}"
            << "bool Add(" << first
            << " value){if(Contains(value))return false;EnsureCapacity(count+1);"
            << "int idx=count;values[idx]=value;count=count+1;"
            << "int b=BucketFor(value,buckets.length);"
            << "nexts[idx]=buckets[b];buckets[b]=idx+1;return true;}"
            << "bool Remove(" << first << " value){if(count==0)return false;"
            << "int bcap=buckets.length;if(bcap==0)return false;"
            << "int b=BucketFor(value,bcap);int prev=-1;int cur=buckets[b]-1;int found=-1;"
            << "while(cur>=0){if(values[cur]==value){found=cur;"
            << "if(prev<0)buckets[b]=nexts[cur];else nexts[prev]=nexts[cur];cur=-1;}"
            << "else{prev=cur;cur=nexts[cur]-1;}}"
            << "if(found<0)return false;"
            << "int last=count-1;"
            << "if(found!=last){" << first << " moved=values[last];values[found]=moved;nexts[found]=nexts[last];"
            << "int mb=BucketFor(moved,bcap);"
            << "if(buckets[mb]==last+1){buckets[mb]=found+1;}else{"
            << "int mcur=buckets[mb]-1;while(mcur>=0){if(nexts[mcur]==last+1){nexts[mcur]=found+1;mcur=-1;}else{mcur=nexts[mcur]-1;}}}}"
            << "values[last]=__defaultItem;nexts[last]=0;count=last;return true;}"
            << first << " Get(int index){return values[index];}"
            << first << "[] ToArray(){" << first << "[] arr=new " << first
            << "[count];int i=0;while(i<count){arr[i]=values[i];i=i+1;}return arr;}"
            << "void Clear(){int i=0;while(i<count){values[i]=__defaultItem;nexts[i]=0;i=i+1;}int bi=0;int bcap=buckets.length;while(bi<bcap){buckets[bi]=0;bi=bi+1;}count=0;return;}"
            << enumerator << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "struct " << enumerator << "{" << generatedName
            << " __owner;int __index;" << enumerator << "(" << generatedName
            << " value){__owner=value;__index=-1;}bool MoveNext(){if(__owner==null)return false;__index=__index+1;"
            << "return __index<__owner.Count();}" << first
            << " Current(){return __owner.Get(__index);}}";
    } else if (name == "Dictionary") {
        if (arguments.size() != 2) return {};
        const auto enumerator = generatedName + "__Enumerator";
        source << "class " << generatedName << "{" << first
            << "[] keys;" << second << "[] values;int[] buckets;int[] nexts;int count;"
            << first << " __defaultKey;" << second << " __defaultValue;"
            << generatedName << "(){keys=new " << first << "[4];values=new " << second
            << "[4];buckets=new int[8];nexts=new int[4];count=0;}" << generatedName
            << "(int capacity){int cap=capacity;if(cap<1)cap=4;"
            << "int bcap=8;while(bcap<cap*2){bcap=bcap*2;}"
            << "keys=new " << first << "[cap];values=new " << second
            << "[cap];buckets=new int[bcap];nexts=new int[cap];count=0;}"
            << "int BucketFor(" << first << " key,int bcap){"
            << "int h=__rs_stable_hash(key);"
            << "if(h==-2147483647-1)h=0;else if(h<0)h=0-h;"
            << "return h%bcap;}"
            << "void Rehash(){int bcap=buckets.length;int bi=0;while(bi<bcap){buckets[bi]=0;bi=bi+1;}"
            << "int i=0;while(i<count){int b=BucketFor(keys[i],bcap);"
            << "nexts[i]=buckets[b];buckets[b]=i+1;i=i+1;}return;}"
            << "void EnsureCapacity(int needed){if(needed<=keys.length)return;"
            << "int next=keys.length;if(next<4)next=4;while(next<needed){next=next*2;}"
            << "int bcap=8;while(bcap<next*2){bcap=bcap*2;}"
            << first << "[] grownKeys=new " << first << "[next];"
            << second << "[] grownValues=new " << second << "[next];int i=0;"
            << "while(i<count){grownKeys[i]=keys[i];grownValues[i]=values[i];i=i+1;}"
            << "keys=grownKeys;values=grownValues;buckets=new int[bcap];nexts=new int[next];Rehash();return;}"
            << "int Count(){return count;}int Capacity(){return keys.length;}"
            << "bool IsEmpty(){return count==0;}"
            << "int Find(" << first << " key){if(count==0)return -1;"
            << "int bcap=buckets.length;if(bcap==0)return -1;"
            << "int cur=buckets[BucketFor(key,bcap)]-1;while(cur>=0){"
            << "if(keys[cur]==key)return cur;cur=nexts[cur]-1;}return -1;}"
            << "bool ContainsKey(" << first
            << " key){return Find(key)>=0;}"
            << "bool ContainsValue(" << second
            << " value){int i=0;while(i<count){if(values[i]==value)return true;"
            << "i=i+1;}return false;}"
            << "bool TryGetValue(" << first << " key,out " << second
            << " value){int i=Find(key);if(i<0){value=__defaultValue;return false;}value=values[i];return true;}"
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
            << "int b=BucketFor(key,buckets.length);"
            << "nexts[idx]=buckets[b];buckets[b]=idx+1;return;}"
            << "void Set(" << first << " key," << second
            << " value){Put(key,value);return;}"
            << "void Add(" << first << " key," << second
            << " value){Put(key,value);return;}"
            << second << " Get(" << first
            << " key){return values[Find(key)];}"
            << second << " GetAt(int index){return values[index];}"
            << first << " GetKeyAt(int index){return keys[index];}"
            << "bool Remove(" << first << " key){if(count==0)return false;"
            << "int bcap=buckets.length;if(bcap==0)return false;"
            << "int b=BucketFor(key,bcap);int prev=-1;int cur=buckets[b]-1;int found=-1;"
            << "while(cur>=0){if(keys[cur]==key){found=cur;"
            << "if(prev<0)buckets[b]=nexts[cur];else nexts[prev]=nexts[cur];cur=-1;}"
            << "else{prev=cur;cur=nexts[cur]-1;}}"
            << "if(found<0)return false;"
            << "int last=count-1;"
            << "if(found!=last){" << first << " movedKey=keys[last];" << second << " movedVal=values[last];"
            << "keys[found]=movedKey;values[found]=movedVal;nexts[found]=nexts[last];"
            << "int mb=BucketFor(movedKey,bcap);"
            << "if(buckets[mb]==last+1){buckets[mb]=found+1;}else{"
            << "int mcur=buckets[mb]-1;while(mcur>=0){if(nexts[mcur]==last+1){nexts[mcur]=found+1;mcur=-1;}else{mcur=nexts[mcur]-1;}}}}"
            << "keys[last]=__defaultKey;values[last]=__defaultValue;nexts[last]=0;count=last;return true;}"
            << "void Clear(){int i=0;while(i<count){keys[i]=__defaultKey;values[i]=__defaultValue;nexts[i]=0;i=i+1;}int bi=0;int bcap=buckets.length;while(bi<bcap){buckets[bi]=0;bi=bi+1;}count=0;return;}"
            << enumerator << " GetEnumerator(){return new " << enumerator << "(this);}}"
            << "struct " << enumerator << "{" << generatedName
            << " __owner;int __index;" << enumerator << "(" << generatedName
            << " value){__owner=value;__index=-1;}bool MoveNext(){if(__owner==null)return false;__index=__index+1;"
            << "return __index<__owner.Count();}" << second
            << " Current(){return __owner.GetAt(__index);}}";
    }
    return source.str();
}


namespace ast_monomorphizer {

using SubstitutionMap = std::map<std::string, HygienicSubstitutedType>;

inline bool& arrayRankOverflowFlag() noexcept {
    thread_local bool flag = false;
    return flag;
}

inline void collectPatternVariablesFromPattern(
    const syntax::PatternSyntax& pattern,
    std::set<std::string>& out,
    bool positiveOnly) {
    switch (pattern.kind()) {
    case syntax::SyntaxKind::VarPattern: {
        const auto& vp = static_cast<const syntax::VarPatternSyntax&>(pattern);
        if (!vp.designationToken.text.empty() && vp.designationToken.text != "_") {
            out.insert(vp.designationToken.text);
        }
        return;
    }
    case syntax::SyntaxKind::TypePattern: {
        const auto& tp = static_cast<const syntax::TypePatternSyntax&>(pattern);
        if (tp.designationToken && !tp.designationToken->text.empty() &&
            tp.designationToken->text != "_") {
            out.insert(tp.designationToken->text);
        }
        return;
    }
    case syntax::SyntaxKind::PropertyPattern: {
        const auto& pp = static_cast<const syntax::PropertyPatternSyntax&>(pattern);
        for (const auto& prop : pp.properties) {
            if (prop.pattern) {
                collectPatternVariablesFromPattern(*prop.pattern, out, positiveOnly);
            }
        }
        if (pp.designationToken && !pp.designationToken->text.empty() &&
            pp.designationToken->text != "_") {
            out.insert(pp.designationToken->text);
        }
        return;
    }
    case syntax::SyntaxKind::ParenthesizedPattern: {
        const auto& paren = static_cast<const syntax::ParenthesizedPatternSyntax&>(pattern);
        if (paren.pattern) collectPatternVariablesFromPattern(*paren.pattern, out, positiveOnly);
        return;
    }
    case syntax::SyntaxKind::BinaryPattern: {
        const auto& bin = static_cast<const syntax::BinaryPatternSyntax&>(pattern);
        if (!positiveOnly || bin.operatorToken.text == "and") {
            if (bin.left) collectPatternVariablesFromPattern(*bin.left, out, positiveOnly);
            if (bin.right) collectPatternVariablesFromPattern(*bin.right, out, positiveOnly);
        }
        return;
    }
    case syntax::SyntaxKind::UnaryPattern: {
        const auto& un = static_cast<const syntax::UnaryPatternSyntax&>(pattern);
        if (!positiveOnly && un.pattern) {
            collectPatternVariablesFromPattern(*un.pattern, out, positiveOnly);
        }
        return;
    }
    default:
        return;
    }
}

inline void collectIntroducedPatternVariables(
    const syntax::ExpressionSyntax& expr,
    std::set<std::string>& out) {
    switch (expr.kind()) {
    case syntax::SyntaxKind::TypeBinaryExpression: {
        const auto& tb = static_cast<const syntax::TypeBinaryExpressionSyntax&>(expr);
        if (tb.expression) collectIntroducedPatternVariables(*tb.expression, out);
        if (tb.pattern) {
            collectPatternVariablesFromPattern(*tb.pattern, out, true);
        } else if (tb.designationToken && !tb.designationToken->text.empty() &&
                   tb.designationToken->text != "_") {
            out.insert(tb.designationToken->text);
        }
        return;
    }
    case syntax::SyntaxKind::ParenthesizedExpression: {
        const auto& p = static_cast<const syntax::ParenthesizedExpressionSyntax&>(expr);
        if (p.expression) collectIntroducedPatternVariables(*p.expression, out);
        return;
    }
    case syntax::SyntaxKind::BinaryExpression: {
        const auto& b = static_cast<const syntax::BinaryExpressionSyntax&>(expr);
        if (b.operatorToken.kind == syntax::SyntaxKind::AmpersandAmpersandToken) {
            if (b.left) collectIntroducedPatternVariables(*b.left, out);
            if (b.right) collectIntroducedPatternVariables(*b.right, out);
        }
        return;
    }
    default:
        return;
    }
}

inline void substituteType(
    syntax::TypeSyntax& type,
    const SubstitutionMap& subs,
    bool& hasGenericSyntax) {
    if (type.isGeneric()) {
        for (auto& arg : type.typeArguments) {
            substituteType(arg, subs, hasGenericSyntax);
        }
        if (stripCanonicalModulePrefix(type.name.text) == "Nullable" &&
            type.typeArguments.size() == 1 &&
            !type.isArray()) {
            const auto& inner = type.typeArguments[0];
            const auto innerSimple = stripCanonicalModulePrefix(inner.name.text);
            if (!inner.isArray() &&
                ((innerSimple == "Nullable" && inner.isGeneric()) ||
                 innerSimple.rfind("Nullable__", 0) == 0)) {
                auto collapsed = std::move(type.typeArguments[0]);
                type = std::move(collapsed);
            }
        }
        hasGenericSyntax = true;
        return;
    }
    const auto found = subs.find(type.name.text);
    if (found == subs.end()) return;
    const auto& sub = found->second;
    const auto origSpan = type.name.span;
    const bool outerArray = type.isArray();
    const auto outerOpenBracket = type.openBracketToken;
    const auto outerCloseBracket = type.closeBracketToken;
    const std::uint32_t totalRank = sub.arrayRank + (outerArray ? 1u : 0u);
    if (totalRank > 1u) {
        arrayRankOverflowFlag() = true;
    }

    type.name.text = sub.canonicalBaseName;
    if (sub.isNullable) {
        auto underlying = type;
        underlying.openBracketToken.reset();
        underlying.closeBracketToken.reset();
        syntax::TypeSyntax wrapped;
        wrapped.name = {syntax::SyntaxKind::IdentifierToken, origSpan, "Nullable", {}, true};
        wrapped.lessToken = {syntax::SyntaxKind::LessToken, origSpan, "<", {}, true};
        wrapped.typeArguments.push_back(std::move(underlying));
        wrapped.greaterToken = {syntax::SyntaxKind::GreaterToken, origSpan, ">", {}, true};
        wrapped.nullableToken = {syntax::SyntaxKind::QuestionToken, origSpan, "?", {}, true};
        type = std::move(wrapped);
        hasGenericSyntax = true;
    }
    if (sub.isArray || outerArray) {
        type.openBracketToken = outerOpenBracket.has_value()
            ? outerOpenBracket
            : syntax::SyntaxToken{syntax::SyntaxKind::OpenBracketToken, origSpan, "[", {}, true};
        type.closeBracketToken = outerCloseBracket.has_value()
            ? outerCloseBracket
            : syntax::SyntaxToken{syntax::SyntaxKind::CloseBracketToken, origSpan, "]", {}, true};
    }
}

inline void substituteExpression(
    syntax::ExpressionSyntax& expression,
    const SubstitutionMap& subs,
    const std::set<std::string>& shadowedValueNames,
    bool& hasGenericSyntax);

inline void substitutePattern(
    syntax::PatternSyntax& pattern,
    const SubstitutionMap& subs,
    const std::set<std::string>& shadowedValueNames,
    bool& hasGenericSyntax) {
    switch (pattern.kind()) {
    case syntax::SyntaxKind::ConstantPattern: {
        auto& cp = static_cast<syntax::ConstantPatternSyntax&>(pattern);
        if (cp.expression) {
            substituteExpression(*cp.expression, subs, shadowedValueNames, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::RelationalPattern: {
        auto& rp = static_cast<syntax::RelationalPatternSyntax&>(pattern);
        if (rp.operand) {
            substituteExpression(*rp.operand, subs, shadowedValueNames, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::TypePattern: {
        auto& tp = static_cast<syntax::TypePatternSyntax&>(pattern);
        const bool shadowedConst =
            !tp.designationToken && !tp.type.isGeneric() &&
            !tp.type.isArray() && !tp.type.isNullable() &&
            shadowedValueNames.find(tp.type.name.text) != shadowedValueNames.end();
        if (!shadowedConst) {
            substituteType(tp.type, subs, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::PropertyPattern: {
        auto& pp = static_cast<syntax::PropertyPatternSyntax&>(pattern);
        if (pp.type) {
            substituteType(*pp.type, subs, hasGenericSyntax);
        }
        for (auto& prop : pp.properties) {
            if (prop.pattern) {
                substitutePattern(*prop.pattern, subs, shadowedValueNames, hasGenericSyntax);
            }
        }
        return;
    }
    case syntax::SyntaxKind::UnaryPattern: {
        auto& un = static_cast<syntax::UnaryPatternSyntax&>(pattern);
        if (un.pattern) {
            substitutePattern(*un.pattern, subs, shadowedValueNames, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::BinaryPattern: {
        auto& bin = static_cast<syntax::BinaryPatternSyntax&>(pattern);
        if (bin.left) substitutePattern(*bin.left, subs, shadowedValueNames, hasGenericSyntax);
        if (bin.right) substitutePattern(*bin.right, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::ParenthesizedPattern: {
        auto& paren = static_cast<syntax::ParenthesizedPatternSyntax&>(pattern);
        if (paren.pattern) {
            substitutePattern(*paren.pattern, subs, shadowedValueNames, hasGenericSyntax);
        }
        return;
    }
    default:
        return;
    }
}

inline void substituteExpression(
    syntax::ExpressionSyntax& expression,
    const SubstitutionMap& subs,
    const std::set<std::string>& shadowedValueNames,
    bool& hasGenericSyntax) {
    switch (expression.kind()) {
    case syntax::SyntaxKind::NameExpression: {
        auto& expr = static_cast<syntax::NameExpressionSyntax&>(expression);
        if (expr.lessToken) {
            for (auto& arg : expr.typeArguments) {
                substituteType(arg, subs, hasGenericSyntax);
            }
            hasGenericSyntax = true;
        } else if (shadowedValueNames.find(expr.identifierToken.text) == shadowedValueNames.end()) {
            const auto found = subs.find(expr.identifierToken.text);
            if (found != subs.end() && !found->second.isArray) {
                expr.identifierToken.text = found->second.canonicalBaseName;
            }
        }
        return;
    }
    case syntax::SyntaxKind::LambdaExpression: {
        auto& expr = static_cast<syntax::LambdaExpressionSyntax&>(expression);
        auto innerShadowed = shadowedValueNames;
        for (const auto& p : expr.parameterTokens) innerShadowed.insert(p.text);
        if (expr.body) substituteExpression(*expr.body, subs, innerShadowed, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::UnaryExpression: {
        auto& expr = static_cast<syntax::UnaryExpressionSyntax&>(expression);
        if (expr.operand) substituteExpression(*expr.operand, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::BinaryExpression: {
        auto& expr = static_cast<syntax::BinaryExpressionSyntax&>(expression);
        if (expr.left) substituteExpression(*expr.left, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.right) {
            if (expr.operatorToken.kind == syntax::SyntaxKind::AmpersandAmpersandToken && expr.left) {
                auto rightShadowed = shadowedValueNames;
                collectIntroducedPatternVariables(*expr.left, rightShadowed);
                substituteExpression(*expr.right, subs, rightShadowed, hasGenericSyntax);
            } else {
                substituteExpression(*expr.right, subs, shadowedValueNames, hasGenericSyntax);
            }
        }
        return;
    }
    case syntax::SyntaxKind::TypeBinaryExpression: {
        auto& expr = static_cast<syntax::TypeBinaryExpressionSyntax&>(expression);
        if (expr.expression) substituteExpression(*expr.expression, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.pattern) {
            substitutePattern(*expr.pattern, subs, shadowedValueNames, hasGenericSyntax);
            if (expr.pattern->kind() == syntax::SyntaxKind::TypePattern) {
                expr.type = static_cast<const syntax::TypePatternSyntax&>(*expr.pattern).type;
            }
        } else {
            substituteType(expr.type, subs, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::TaggedStringExpression:
        return;
    case syntax::SyntaxKind::TypeOfExpression: {
        auto& expr = static_cast<syntax::TypeOfExpressionSyntax&>(expression);
        substituteType(expr.type, subs, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::SwitchExpression: {
        auto& expr = static_cast<syntax::SwitchExpressionSyntax&>(expression);
        if (expr.expression) substituteExpression(*expr.expression, subs, shadowedValueNames, hasGenericSyntax);
        for (auto& arm : expr.arms) {
            auto armShadowed = shadowedValueNames;
            if (arm.pattern) {
                substitutePattern(*arm.pattern, subs, armShadowed, hasGenericSyntax);
                collectPatternVariablesFromPattern(*arm.pattern, armShadowed, false);
                if (arm.pattern->kind() == syntax::SyntaxKind::TypePattern) {
                    arm.patternType = static_cast<const syntax::TypePatternSyntax&>(*arm.pattern).type;
                }
            } else {
                if (arm.patternType) substituteType(*arm.patternType, subs, hasGenericSyntax);
                if (arm.patternDesignation) armShadowed.insert(arm.patternDesignation->text);
                if (arm.label) substituteExpression(*arm.label, subs, armShadowed, hasGenericSyntax);
            }
            if (arm.guard) substituteExpression(*arm.guard, subs, armShadowed, hasGenericSyntax);
            if (arm.value) substituteExpression(*arm.value, subs, armShadowed, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::ConditionalExpression: {
        auto& expr = static_cast<syntax::ConditionalExpressionSyntax&>(expression);
        if (expr.condition) substituteExpression(*expr.condition, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.whenTrue) substituteExpression(*expr.whenTrue, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.whenFalse) substituteExpression(*expr.whenFalse, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::AssignmentExpression: {
        auto& expr = static_cast<syntax::AssignmentExpressionSyntax&>(expression);
        if (expr.expression) substituteExpression(*expr.expression, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::ParenthesizedExpression: {
        auto& expr = static_cast<syntax::ParenthesizedExpressionSyntax&>(expression);
        if (expr.expression) substituteExpression(*expr.expression, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::CastExpression: {
        auto& expr = static_cast<syntax::CastExpressionSyntax&>(expression);
        substituteType(expr.type, subs, hasGenericSyntax);
        if (expr.expression) substituteExpression(*expr.expression, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::CallExpression: {
        auto& expr = static_cast<syntax::CallExpressionSyntax&>(expression);
        if (expr.lessToken) {
            for (auto& arg : expr.typeArguments) {
                substituteType(arg, subs, hasGenericSyntax);
            }
            hasGenericSyntax = true;
        }
        for (auto& arg : expr.arguments) {
            if (arg) substituteExpression(*arg, subs, shadowedValueNames, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::MemberCallExpression: {
        auto& expr = static_cast<syntax::MemberCallExpressionSyntax&>(expression);
        if (expr.receiver) substituteExpression(*expr.receiver, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.lessToken) {
            for (auto& arg : expr.typeArguments) {
                substituteType(arg, subs, hasGenericSyntax);
            }
            hasGenericSyntax = true;
        }
        for (auto& arg : expr.arguments) {
            if (arg) substituteExpression(*arg, subs, shadowedValueNames, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::MemberAccessExpression: {
        auto& expr = static_cast<syntax::MemberAccessExpressionSyntax&>(expression);
        if (expr.receiver) substituteExpression(*expr.receiver, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::ElementAccessExpression: {
        auto& expr = static_cast<syntax::ElementAccessExpressionSyntax&>(expression);
        if (expr.receiver) substituteExpression(*expr.receiver, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.index) substituteExpression(*expr.index, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::ElementAssignmentExpression: {
        auto& expr = static_cast<syntax::ElementAssignmentExpressionSyntax&>(expression);
        if (expr.receiver) substituteExpression(*expr.receiver, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.index) substituteExpression(*expr.index, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.expression) substituteExpression(*expr.expression, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::MemberAssignmentExpression: {
        auto& expr = static_cast<syntax::MemberAssignmentExpressionSyntax&>(expression);
        if (expr.receiver) substituteExpression(*expr.receiver, subs, shadowedValueNames, hasGenericSyntax);
        if (expr.expression) substituteExpression(*expr.expression, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::NewObjectExpression: {
        auto& expr = static_cast<syntax::NewObjectExpressionSyntax&>(expression);
        substituteType(expr.type, subs, hasGenericSyntax);
        for (auto& arg : expr.arguments) {
            if (arg) substituteExpression(*arg, subs, shadowedValueNames, hasGenericSyntax);
        }
        for (auto& init : expr.initializers) {
            for (auto& e : init.expressions) {
                if (e) substituteExpression(*e, subs, shadowedValueNames, hasGenericSyntax);
            }
        }
        return;
    }
    case syntax::SyntaxKind::NewArrayExpression: {
        auto& expr = static_cast<syntax::NewArrayExpressionSyntax&>(expression);
        substituteType(expr.elementType, subs, hasGenericSyntax);
        if (expr.length) substituteExpression(*expr.length, subs, shadowedValueNames, hasGenericSyntax);
        for (auto& item : expr.initialValues) {
            if (item) substituteExpression(*item, subs, shadowedValueNames, hasGenericSyntax);
        }
        return;
    }
    default:
        return;
    }
}

inline void substituteStatement(
    syntax::StatementSyntax& statement,
    const SubstitutionMap& subs,
    std::set<std::string>& shadowedValueNames,
    bool& hasGenericSyntax) {
    switch (statement.kind()) {
    case syntax::SyntaxKind::BlockStatement: {
        auto blockShadowed = shadowedValueNames;
        for (auto& child : static_cast<syntax::BlockStatementSyntax&>(statement).statements) {
            if (child) substituteStatement(*child, subs, blockShadowed, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::ReturnStatement: {
        auto& stmt = static_cast<syntax::ReturnStatementSyntax&>(statement);
        if (stmt.expression) substituteExpression(*stmt.expression, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::IfStatement: {
        auto& stmt = static_cast<syntax::IfStatementSyntax&>(statement);
        if (stmt.condition) substituteExpression(*stmt.condition, subs, shadowedValueNames, hasGenericSyntax);
        if (stmt.thenStatement) {
            auto thenShadowed = shadowedValueNames;
            if (stmt.condition) collectIntroducedPatternVariables(*stmt.condition, thenShadowed);
            substituteStatement(*stmt.thenStatement, subs, thenShadowed, hasGenericSyntax);
        }
        if (stmt.elseStatement) {
            auto elseShadowed = shadowedValueNames;
            substituteStatement(*stmt.elseStatement, subs, elseShadowed, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::WhileStatement: {
        auto& stmt = static_cast<syntax::WhileStatementSyntax&>(statement);
        if (stmt.condition) substituteExpression(*stmt.condition, subs, shadowedValueNames, hasGenericSyntax);
        if (stmt.body) {
            auto bodyShadowed = shadowedValueNames;
            if (stmt.condition) collectIntroducedPatternVariables(*stmt.condition, bodyShadowed);
            substituteStatement(*stmt.body, subs, bodyShadowed, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::ForStatement: {
        auto& stmt = static_cast<syntax::ForStatementSyntax&>(statement);
        auto forShadowed = shadowedValueNames;
        if (stmt.initializer) substituteStatement(*stmt.initializer, subs, forShadowed, hasGenericSyntax);
        if (stmt.condition) substituteExpression(*stmt.condition, subs, forShadowed, hasGenericSyntax);
        if (stmt.increment) substituteExpression(*stmt.increment, subs, forShadowed, hasGenericSyntax);
        if (stmt.body) substituteStatement(*stmt.body, subs, forShadowed, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::ForeachStatement: {
        auto& stmt = static_cast<syntax::ForeachStatementSyntax&>(statement);
        substituteType(stmt.type, subs, hasGenericSyntax);
        for (auto& extra : stmt.additionalBindings) {
            substituteType(extra.type, subs, hasGenericSyntax);
        }
        if (stmt.collection) substituteExpression(*stmt.collection, subs, shadowedValueNames, hasGenericSyntax);
        auto bodyShadowed = shadowedValueNames;
        bodyShadowed.insert(stmt.identifierToken.text);
        for (const auto& extra : stmt.additionalBindings) {
            bodyShadowed.insert(extra.identifierToken.text);
        }
        if (stmt.body) substituteStatement(*stmt.body, subs, bodyShadowed, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::DoWhileStatement: {
        auto& stmt = static_cast<syntax::DoWhileStatementSyntax&>(statement);
        if (stmt.body) {
            auto bodyShadowed = shadowedValueNames;
            substituteStatement(*stmt.body, subs, bodyShadowed, hasGenericSyntax);
        }
        if (stmt.condition) substituteExpression(*stmt.condition, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::SwitchStatement: {
        auto& stmt = static_cast<syntax::SwitchStatementSyntax&>(statement);
        if (stmt.expression) substituteExpression(*stmt.expression, subs, shadowedValueNames, hasGenericSyntax);
        for (auto& sec : stmt.sections) {
            auto secShadowed = shadowedValueNames;
            if (sec.pattern) {
                substitutePattern(*sec.pattern, subs, secShadowed, hasGenericSyntax);
                collectPatternVariablesFromPattern(*sec.pattern, secShadowed, false);
                if (sec.pattern->kind() == syntax::SyntaxKind::TypePattern) {
                    sec.patternType = static_cast<const syntax::TypePatternSyntax&>(*sec.pattern).type;
                }
            } else {
                if (sec.patternType) substituteType(*sec.patternType, subs, hasGenericSyntax);
                if (sec.patternDesignation) secShadowed.insert(sec.patternDesignation->text);
                if (sec.label) substituteExpression(*sec.label, subs, secShadowed, hasGenericSyntax);
            }
            if (sec.guard) substituteExpression(*sec.guard, subs, secShadowed, hasGenericSyntax);
            for (auto& child : sec.statements) {
                if (child) substituteStatement(*child, subs, secShadowed, hasGenericSyntax);
            }
        }
        return;
    }
    case syntax::SyntaxKind::ThrowStatement: {
        auto& stmt = static_cast<syntax::ThrowStatementSyntax&>(statement);
        if (stmt.expression) substituteExpression(*stmt.expression, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::TryStatement: {
        auto& stmt = static_cast<syntax::TryStatementSyntax&>(statement);
        {
            auto tryShadowed = shadowedValueNames;
            substituteStatement(stmt.body, subs, tryShadowed, hasGenericSyntax);
        }
        for (auto& c : stmt.catches) {
            if (c.type) substituteType(*c.type, subs, hasGenericSyntax);
            auto catchShadowed = shadowedValueNames;
            if (c.identifierToken) catchShadowed.insert(c.identifierToken->text);
            substituteStatement(c.body, subs, catchShadowed, hasGenericSyntax);
        }
        if (stmt.finallyBody) {
            auto finShadowed = shadowedValueNames;
            substituteStatement(*stmt.finallyBody, subs, finShadowed, hasGenericSyntax);
        }
        return;
    }
    case syntax::SyntaxKind::YieldWaitStatement: {
        auto& stmt = static_cast<syntax::YieldWaitStatementSyntax&>(statement);
        if (stmt.delay) substituteExpression(*stmt.delay, subs, shadowedValueNames, hasGenericSyntax);
        if (stmt.secondArgument) substituteExpression(*stmt.secondArgument, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::EventSubscriptionStatement: {
        auto& stmt = static_cast<syntax::EventSubscriptionStatementSyntax&>(statement);
        if (stmt.receiver) substituteExpression(*stmt.receiver, subs, shadowedValueNames, hasGenericSyntax);
        if (stmt.handler) substituteExpression(*stmt.handler, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    case syntax::SyntaxKind::VariableDeclarationStatement: {
        auto& stmt = static_cast<syntax::VariableDeclarationStatementSyntax&>(statement);
        substituteType(stmt.type, subs, hasGenericSyntax);
        if (stmt.initializer) substituteExpression(*stmt.initializer, subs, shadowedValueNames, hasGenericSyntax);
        shadowedValueNames.insert(stmt.identifierToken.text);
        return;
    }
    case syntax::SyntaxKind::ExpressionStatement: {
        auto& stmt = static_cast<syntax::ExpressionStatementSyntax&>(statement);
        if (stmt.expression) substituteExpression(*stmt.expression, subs, shadowedValueNames, hasGenericSyntax);
        return;
    }
    default:
        return;
    }
}

inline void substituteFunctionDeclaration(
    syntax::FunctionDeclarationSyntax& fn,
    const SubstitutionMap& outerSubs,
    const std::set<std::string>& memberNames,
    bool& hasGenericSyntax) {
    SubstitutionMap activeSubs = outerSubs;
    for (const auto& ownTp : fn.typeParameters) {
        activeSubs.erase(ownTp.text);
    }
    if (!fn.typeParameters.empty()) {
        hasGenericSyntax = true;
    }
    substituteType(fn.returnType, activeSubs, hasGenericSyntax);
    auto shadowed = memberNames;
    for (auto& param : fn.parameters) {
        substituteType(param.type, activeSubs, hasGenericSyntax);
        if (param.defaultValue) {
            substituteExpression(*param.defaultValue, activeSubs, shadowed, hasGenericSyntax);
        }
        shadowed.insert(param.identifierToken.text);
    }
    if (fn.hasBody()) {
        substituteStatement(fn.body, activeSubs, shadowed, hasGenericSyntax);
    }
}

template <typename TypeDecl>
inline void substituteTypeLikeDeclaration(
    TypeDecl& decl,
    const std::string& originalName,
    const std::string& generatedName,
    const SubstitutionMap& subs,
    bool& hasGenericSyntax) {
    decl.identifierToken.text = generatedName;
    decl.typeParameterLessToken.reset();
    decl.typeParameters.clear();
    decl.typeParameterCommaTokens.clear();
    decl.typeParameterGreaterToken.reset();
    decl.constraints.clear();

    for (auto& iface : decl.interfaces) {
        substituteType(iface, subs, hasGenericSyntax);
    }
    std::set<std::string> memberNames;
    for (const auto& f : decl.fields) memberNames.insert(f.identifierToken.text);
    for (const auto& p : decl.properties) memberNames.insert(p.identifierToken.text);
    for (const auto& e : decl.events) memberNames.insert(e.identifierToken.text);

    for (auto& f : decl.fields) {
        substituteType(f.type, subs, hasGenericSyntax);
        if (f.initializer) {
            substituteExpression(*f.initializer, subs, memberNames, hasGenericSyntax);
        }
    }
    for (auto& e : decl.events) {
        substituteType(e.delegateType, subs, hasGenericSyntax);
    }
    for (auto& ctor : decl.constructors) {
        if (ctor.identifierToken.text == originalName) {
            ctor.identifierToken.text = generatedName;
        }
        auto shadowed = memberNames;
        for (auto& param : ctor.parameters) {
            substituteType(param.type, subs, hasGenericSyntax);
            if (param.defaultValue) {
                substituteExpression(*param.defaultValue, subs, shadowed, hasGenericSyntax);
            }
            shadowed.insert(param.identifierToken.text);
        }
        for (auto& baseArg : ctor.baseArguments) {
            if (baseArg) substituteExpression(*baseArg, subs, shadowed, hasGenericSyntax);
        }
        substituteStatement(ctor.body, subs, shadowed, hasGenericSyntax);
    }
    for (auto& prop : decl.properties) {
        substituteType(prop.type, subs, hasGenericSyntax);
        if (prop.getter && prop.getter->body) {
            auto shadowed = memberNames;
            substituteStatement(*prop.getter->body, subs, shadowed, hasGenericSyntax);
        }
        if (prop.setter && prop.setter->body) {
            auto shadowed = memberNames;
            shadowed.insert("value");
            substituteStatement(*prop.setter->body, subs, shadowed, hasGenericSyntax);
        }
        if (prop.initializer) {
            substituteExpression(*prop.initializer, subs, memberNames, hasGenericSyntax);
        }
    }
    for (auto& seq : decl.sequences) {
        if (seq.resultType) substituteType(*seq.resultType, subs, hasGenericSyntax);
        auto shadowed = memberNames;
        for (auto& param : seq.parameters) {
            substituteType(param.type, subs, hasGenericSyntax);
            if (param.defaultValue) {
                substituteExpression(*param.defaultValue, subs, shadowed, hasGenericSyntax);
            }
            shadowed.insert(param.identifierToken.text);
        }
        substituteStatement(seq.body, subs, shadowed, hasGenericSyntax);
    }
    for (auto& method : decl.methods) {
        substituteFunctionDeclaration(method, subs, memberNames, hasGenericSyntax);
    }
}

inline void substituteCompilationUnit(
    syntax::CompilationUnitSyntax& unit,
    const SubstitutionMap& subs,
    bool& hasGenericSyntax) {
    for (auto& cls : unit.classes) {
        const auto name = cls.identifierToken.text;
        substituteTypeLikeDeclaration(cls, name, name, subs, hasGenericSyntax);
    }
    for (auto& st : unit.structs) {
        const auto name = st.identifierToken.text;
        substituteTypeLikeDeclaration(st, name, name, subs, hasGenericSyntax);
    }
    for (auto& iface : unit.interfaces) {
        std::set<std::string> ifaceMembers;
        for (const auto& m : iface.methods) ifaceMembers.insert(m.identifierToken.text);
        for (auto& m : iface.methods) {
            substituteType(m.returnType, subs, hasGenericSyntax);
            auto shadowed = ifaceMembers;
            for (auto& p : m.parameters) {
                substituteType(p.type, subs, hasGenericSyntax);
                if (p.defaultValue) {
                    substituteExpression(*p.defaultValue, subs, shadowed, hasGenericSyntax);
                }
                shadowed.insert(p.identifierToken.text);
            }
            if (m.hasBody()) {
                substituteStatement(*m.body, subs, shadowed, hasGenericSyntax);
            }
        }
    }
    for (auto& del : unit.delegates) {
        substituteType(del.returnType, subs, hasGenericSyntax);
        for (auto& p : del.parameters) substituteType(p.type, subs, hasGenericSyntax);
    }
    std::set<std::string> emptyMembers;
    for (auto& fn : unit.functions) {
        substituteFunctionDeclaration(fn, subs, emptyMembers, hasGenericSyntax);
    }
}

} // namespace ast_monomorphizer

class NativeGenericSpecializer {
public:
    NativeGenericSpecializer(
        std::vector<std::unique_ptr<ParsedUnit>>& units,
        std::map<std::string, ModuleWork>& modules,
        BuildResult& result)
        : units_(units), modules_(modules), result_(result) {}

    void run() {
        bool anyGenericSyntax = false;
        for (const auto& u : units_) {
            if (u && u->hasGenericSyntax) {
                anyGenericSyntax = true;
                break;
            }
        }
        if (!anyGenericSyntax) return;
        for (auto& u : units_) {
            if (u) detachSyntaxTree(*u);
        }
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
                    NativeGenericTemplate tmpl;
                    tmpl.classSyntax = std::make_shared<syntax::ClassDeclarationSyntax>(
                        syntax::cloneClassDeclarationSyntax(declaration));
                    registerTypeTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.constraints,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Class,
                        std::move(tmpl));
                }
                for (const auto& declaration : unit->syntaxTree->structs) {
                    if (declaration.typeParameters.empty()) continue;
                    NativeGenericTemplate tmpl;
                    tmpl.structSyntax = std::make_shared<syntax::StructDeclarationSyntax>(
                        syntax::cloneStructDeclarationSyntax(declaration));
                    registerTypeTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.constraints,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Struct,
                        std::move(tmpl));
                }
                for (const auto& declaration : unit->syntaxTree->interfaces) {
                    if (declaration.typeParameters.empty()) continue;
                    NativeGenericTemplate tmpl;
                    tmpl.interfaceSyntax = std::make_shared<syntax::InterfaceDeclarationSyntax>(
                        syntax::cloneInterfaceDeclarationSyntax(declaration));
                    registerTypeTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.constraints,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Interface,
                        std::move(tmpl));
                }
                for (const auto& declaration : unit->syntaxTree->delegates) {
                    if (declaration.typeParameters.empty()) continue;
                    NativeGenericTemplate tmpl;
                    tmpl.delegateSyntax = std::make_shared<syntax::DelegateDeclarationSyntax>(
                        syntax::cloneDelegateDeclarationSyntax(declaration));
                    registerTypeTemplate(
                        moduleName,
                        declaration.identifierToken.text,
                        declaration.typeParameters,
                        declaration.constraints,
                        declaration.span(),
                        unit,
                        NativeGenericTemplate::Kind::Delegate,
                        std::move(tmpl));
                }
                for (const auto& declaration : unit->syntaxTree->functions) {
                    if (declaration.typeParameters.empty()) continue;
                    addFunctionTemplate(moduleName, declaration, unit);
                }
                for (const auto& declaration : unit->syntaxTree->classes) {
                    if (!declaration.typeParameters.empty()) continue;
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
                    if (!declaration.typeParameters.empty()) continue;
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

    void registerTypeTemplate(
        const std::string& moduleName,
        const std::string& name,
        const std::vector<syntax::SyntaxToken>& parameters,
        const std::vector<syntax::GenericConstraintClauseSyntax>& constraints,
        text::TextSpan span,
        const ParsedUnit* unit,
        NativeGenericTemplate::Kind kind,
        NativeGenericTemplate declaration) {
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
        declaration.functionSyntax = std::make_shared<syntax::FunctionDeclarationSyntax>(
            syntax::cloneFunctionDeclarationSyntax(function));
        for (const auto& parameter : function.typeParameters) {
            declaration.parameters.push_back(parameter.text);
        }
        declaration.returnTypeSyntax = function.returnType;
        declaration.isExtension = !function.parameters.empty() &&
            function.parameters.front().hasThisModifier();
        for (const auto& parameter : function.parameters) {
            declaration.parameterTypes.push_back(
                nativeGenericTypeText(parameter.type));
            declaration.parameterTypeSyntaxes.push_back(parameter.type);
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
        declaration.functionSyntax = std::make_shared<syntax::FunctionDeclarationSyntax>(
            syntax::cloneFunctionDeclarationSyntax(function));
        for (const auto& parameter : function.typeParameters) {
            declaration.parameters.push_back(parameter.text);
        }
        declaration.returnTypeSyntax = function.returnType;
        declaration.isExtension = !function.parameters.empty() &&
            function.parameters.front().hasThisModifier();
        for (const auto& parameter : function.parameters) {
            declaration.parameterTypes.push_back(
                nativeGenericTypeText(parameter.type));
            declaration.parameterTypeSyntaxes.push_back(parameter.type);
        }
        declaration.constraints = nativeGenericConstraints(function.constraints);
        registry_.members[moduleName][function.identifierToken.text]
            .push_back(std::move(declaration));
    }

    void registerDynamicMemberTemplatesFromType(
        const std::string& moduleName,
        const ParsedUnit* unit,
        const syntax::ClassDeclarationSyntax& cls) {
        for (const auto& method : cls.methods) {
            if (!method.typeParameters.empty()) {
                addMemberTemplate(moduleName, cls.identifierToken.text, method, unit);
            }
        }
    }

    void registerDynamicMemberTemplatesFromStruct(
        const std::string& moduleName,
        const ParsedUnit* unit,
        const syntax::StructDeclarationSyntax& st) {
        for (const auto& method : st.methods) {
            if (!method.typeParameters.empty()) {
                addMemberTemplate(moduleName, st.identifierToken.text, method, unit);
            }
        }
    }

    const NativeGenericTemplate* resolveMember(
        const std::string& moduleName,
        const std::string& name,
        const std::string& receiverType,
        text::TextSpan span,
        const std::string& sourceName) {
        const auto simpleReceiver = stripCanonicalModulePrefix(receiverType);
        std::vector<const NativeGenericTemplate*> candidates;
        const auto append = [&](const std::string& candidateModule) {
            const auto module = registry_.members.find(candidateModule);
            if (module == registry_.members.end()) return;
            const auto found = module->second.find(name);
            if (found == module->second.end()) return;
            for (const auto& value : found->second) {
                if (simpleReceiver.empty() || simpleReceiver == value.ownerName) {
                    candidates.push_back(&value);
                }
            }
        };
        append(moduleName);
        if (candidates.empty() || simpleReceiver.empty()) {
            const auto module = modules_.find(moduleName);
            if (module != modules_.end()) {
                for (const auto& imported : module->second.imports) append(imported);
            }
            if (currentRewritingUnit_) {
                for (const auto& hyg : currentRewritingUnit_->unitHygienicModules) append(hyg);
            }
            for (const auto& hyg : currentMethodHygienicModules_) append(hyg);
        }
        if (candidates.size() == 1) return candidates.front();
        if (candidates.size() > 1) {
            std::vector<const NativeGenericTemplate*> ownCandidates;
            for (const auto* c : candidates) {
                if (c->moduleName == moduleName) ownCandidates.push_back(c);
            }
            if (ownCandidates.size() == 1) return ownCandidates.front();
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
        const auto sep = name.find("::");
        if (sep != std::string::npos) {
            const auto explicitMod = name.substr(0, sep);
            const auto simple = name.substr(sep + 2);
            const auto modIt = tables.find(explicitMod);
            if (modIt != tables.end()) {
                const auto found = modIt->second.find(simple);
                if (found != modIt->second.end()) return &found->second;
            }
            return nullptr;
        }
        const auto own = tables.find(moduleName);
        if (own != tables.end()) {
            const auto found = own->second.find(name);
            if (found != own->second.end()) return &found->second;
        }
        std::vector<const NativeGenericTemplate*> candidates;
        std::set<std::string> candidateModules;
        const auto module = modules_.find(moduleName);
        if (module != modules_.end()) {
            candidateModules.insert(
                module->second.imports.begin(), module->second.imports.end());
        }
        if (currentRewritingUnit_) {
            candidateModules.insert(
                currentRewritingUnit_->unitHygienicModules.begin(),
                currentRewritingUnit_->unitHygienicModules.end());
        }
        candidateModules.insert(
            currentMethodHygienicModules_.begin(),
            currentMethodHygienicModules_.end());
        for (const auto& importedName : candidateModules) {
            if (importedName == moduleName) continue;
            const auto imported = tables.find(importedName);
            if (imported == tables.end()) continue;
            const auto found = imported->second.find(name);
            if (found != imported->second.end()) {
                candidates.push_back(&found->second);
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

    std::string findFieldOrPropertyType(
        const std::string& rawOwnerType,
        const std::string& memberName) const {
        if (rawOwnerType.empty()) return {};
        if (rawOwnerType == "string" && (memberName == "Length" || memberName == "Count")) {
            return "int";
        }
        if (rawOwnerType.size() > 2 && rawOwnerType.substr(rawOwnerType.size() - 2) == "[]" &&
            (memberName == "Length" || memberName == "Count" || memberName == "length")) {
            return "int";
        }
        const auto simpleOwner = stripCanonicalModulePrefix(rawOwnerType);
        for (const auto& unit : units_) {
            if (!unit || !unit->syntaxTree) continue;
            for (const auto& cls : unit->syntaxTree->classes) {
                if (!cls.typeParameters.empty() || cls.identifierToken.text != simpleOwner) continue;
                for (const auto& f : cls.fields) {
                    if (f.identifierToken.text == memberName) return typeText(f.type);
                }
                for (const auto& p : cls.properties) {
                    if (p.identifierToken.text == memberName) return typeText(p.type);
                }
            }
            for (const auto& st : unit->syntaxTree->structs) {
                if (!st.typeParameters.empty() || st.identifierToken.text != simpleOwner) continue;
                for (const auto& f : st.fields) {
                    if (f.identifierToken.text == memberName) return typeText(f.type);
                }
                for (const auto& p : st.properties) {
                    if (p.identifierToken.text == memberName) return typeText(p.type);
                }
            }
            for (const auto& en : unit->syntaxTree->enums) {
                if (en.identifierToken.text == simpleOwner) {
                    for (const auto& em : en.members) {
                        if (em.identifierToken.text == memberName) return rawOwnerType;
                    }
                }
            }
        }
        return {};
    }

    std::string findKnownCallableReturnType(
        const std::string& rawOwnerType,
        const std::string& callName,
        std::size_t argCount,
        bool isMemberCall) const {
        const auto simpleCall = stripCanonicalModulePrefix(callName);
        if (const auto specIt = specializedReturnTypes_.find(simpleCall);
            specIt != specializedReturnTypes_.end()) {
            return specIt->second;
        }
        const auto simpleOwner = stripCanonicalModulePrefix(rawOwnerType);
        for (const auto& unit : units_) {
            if (!unit || !unit->syntaxTree) continue;
            if (isMemberCall && !simpleOwner.empty()) {
                for (const auto& cls : unit->syntaxTree->classes) {
                    if (!cls.typeParameters.empty() || cls.identifierToken.text != simpleOwner) continue;
                    for (const auto& m : cls.methods) {
                        if (m.typeParameters.empty() && m.identifierToken.text == simpleCall &&
                            m.parameters.size() == argCount) {
                            return typeText(m.returnType);
                        }
                    }
                }
                for (const auto& st : unit->syntaxTree->structs) {
                    if (!st.typeParameters.empty() || st.identifierToken.text != simpleOwner) continue;
                    for (const auto& m : st.methods) {
                        if (m.typeParameters.empty() && m.identifierToken.text == simpleCall &&
                            m.parameters.size() == argCount) {
                            return typeText(m.returnType);
                        }
                    }
                }
                for (const auto& iface : unit->syntaxTree->interfaces) {
                    if (!iface.typeParameters.empty() || iface.identifierToken.text != simpleOwner) continue;
                    for (const auto& m : iface.methods) {
                        if (m.typeParameters.empty() && m.identifierToken.text == simpleCall &&
                            m.parameters.size() == argCount) {
                            return typeText(m.returnType);
                        }
                    }
                }
            }
            for (const auto& fn : unit->syntaxTree->functions) {
                if (!fn.typeParameters.empty() || fn.identifierToken.text != simpleCall) continue;
                const std::size_t expected = (isMemberCall && !fn.parameters.empty() && fn.parameters.front().hasThisModifier())
                    ? (argCount + 1)
                    : argCount;
                if (fn.parameters.size() == expected) {
                    return typeText(fn.returnType);
                }
            }
            if (isMemberCall) {
                for (const auto& cls : unit->syntaxTree->classes) {
                    if (!cls.typeParameters.empty()) continue;
                    for (const auto& m : cls.methods) {
                        if (!m.typeParameters.empty() || m.identifierToken.text != simpleCall) continue;
                        if (!m.parameters.empty() && m.parameters.front().hasThisModifier() &&
                            m.parameters.size() == argCount + 1) {
                            return typeText(m.returnType);
                        }
                        if (simpleOwner == cls.identifierToken.text && m.parameters.size() == argCount) {
                            return typeText(m.returnType);
                        }
                    }
                }
            }
        }
        return {};
    }

    std::string expressionType(const syntax::ExpressionSyntax& expression) const {
        switch (expression.kind()) {
        case syntax::SyntaxKind::ThisExpression: {
            const auto found = localTypes_.find("this");
            return found == localTypes_.end() ? std::string{} : found->second;
        }
        case syntax::SyntaxKind::NameExpression: {
            const auto& name = static_cast<const
                syntax::NameExpressionSyntax&>(expression).identifierToken.text;
            const auto found = localTypes_.find(name);
            return found == localTypes_.end() ? std::string{} : found->second;
        }
        case syntax::SyntaxKind::LiteralExpression: {
            const auto& tok = static_cast<const
                syntax::LiteralExpressionSyntax&>(expression).literalToken;
            if (tok.kind == syntax::SyntaxKind::IntegerLiteralToken) {
                const bool hasU = tok.text.find('u') != std::string::npos ||
                    tok.text.find('U') != std::string::npos;
                const bool hasL = tok.text.find('l') != std::string::npos ||
                    tok.text.find('L') != std::string::npos;
                if (hasU && hasL) return "ulong";
                if (hasU) return "uint";
                if (hasL) return "long";
                return "int";
            }
            if (tok.kind == syntax::SyntaxKind::FloatLiteralToken) {
                return (!tok.text.empty() && (tok.text.back() == 'f' || tok.text.back() == 'F'))
                    ? "float"
                    : "double";
            }
            if (tok.kind == syntax::SyntaxKind::CharLiteralToken) return "char";
            if (tok.kind == syntax::SyntaxKind::StringLiteralToken) return "string";
            if (tok.kind == syntax::SyntaxKind::TrueKeyword ||
                tok.kind == syntax::SyntaxKind::FalseKeyword) return "bool";
            return {};
        }
        case syntax::SyntaxKind::ParenthesizedExpression:
            return expressionType(*static_cast<const
                syntax::ParenthesizedExpressionSyntax&>(expression).expression);
        case syntax::SyntaxKind::CastExpression:
            return typeText(static_cast<const
                syntax::CastExpressionSyntax&>(expression).type);
        case syntax::SyntaxKind::UnaryExpression: {
            const auto& un = static_cast<const syntax::UnaryExpressionSyntax&>(expression);
            if (un.operatorToken.kind == syntax::SyntaxKind::BangToken) return "bool";
            if (!un.operand) return {};
            auto inner = expressionType(*un.operand);
            if (un.operatorToken.kind == syntax::SyntaxKind::MinusToken ||
                un.operatorToken.kind == syntax::SyntaxKind::PlusToken) {
                if (inner == "byte" || inner == "sbyte" || inner == "short" ||
                    inner == "ushort" || inner == "char") return "int";
                if (inner == "uint") return "long";
            }
            return inner;
        }
        case syntax::SyntaxKind::BinaryExpression: {
            const auto& bin = static_cast<const syntax::BinaryExpressionSyntax&>(expression);
            switch (bin.operatorToken.kind) {
            case syntax::SyntaxKind::EqualsEqualsToken:
            case syntax::SyntaxKind::BangEqualsToken:
            case syntax::SyntaxKind::LessToken:
            case syntax::SyntaxKind::LessOrEqualsToken:
            case syntax::SyntaxKind::GreaterToken:
            case syntax::SyntaxKind::GreaterOrEqualsToken:
            case syntax::SyntaxKind::AmpersandAmpersandToken:
            case syntax::SyntaxKind::PipePipeToken:
                return "bool";
            default:
                break;
            }
            const auto lt = bin.left ? expressionType(*bin.left) : std::string{};
            const auto rt = bin.right ? expressionType(*bin.right) : std::string{};
            if (bin.operatorToken.kind == syntax::SyntaxKind::PlusToken &&
                (lt == "string" || rt == "string")) {
                return "string";
            }
            if (bin.operatorToken.kind == syntax::SyntaxKind::QuestionQuestionToken) {
                return !rt.empty() ? rt : lt;
            }
            if (lt == "double" || rt == "double") return "double";
            if (lt == "float" || rt == "float") return "float";
            if (lt == "ulong" || rt == "ulong") return "ulong";
            if (lt == "long" || rt == "long") return "long";
            if (lt == "uint" || rt == "uint") return "long";
            if (!lt.empty()) return lt;
            return rt;
        }
        case syntax::SyntaxKind::TypeBinaryExpression: {
            const auto& tb = static_cast<const syntax::TypeBinaryExpressionSyntax&>(expression);
            if (tb.operatorToken.kind == syntax::SyntaxKind::AsKeyword) {
                return typeText(tb.type);
            }
            return "bool";
        }
        case syntax::SyntaxKind::ConditionalExpression: {
            const auto& cond = static_cast<const syntax::ConditionalExpressionSyntax&>(expression);
            auto tt = cond.whenTrue ? expressionType(*cond.whenTrue) : std::string{};
            if (!tt.empty()) return tt;
            return cond.whenFalse ? expressionType(*cond.whenFalse) : std::string{};
        }
        case syntax::SyntaxKind::MemberAccessExpression: {
            const auto& ma = static_cast<const syntax::MemberAccessExpressionSyntax&>(expression);
            std::string recvType = ma.receiver ? expressionType(*ma.receiver) : std::string{};
            if (recvType.empty() && ma.receiver &&
                ma.receiver->kind() == syntax::SyntaxKind::NameExpression) {
                recvType = static_cast<const syntax::NameExpressionSyntax&>(
                    *ma.receiver).identifierToken.text;
            }
            return findFieldOrPropertyType(recvType, ma.nameToken.text);
        }
        case syntax::SyntaxKind::ElementAccessExpression: {
            const auto& ea = static_cast<const syntax::ElementAccessExpressionSyntax&>(expression);
            const auto recvType = ea.receiver ? expressionType(*ea.receiver) : std::string{};
            if (recvType.size() > 2 && recvType.substr(recvType.size() - 2) == "[]") {
                return recvType.substr(0, recvType.size() - 2);
            }
            return {};
        }
        case syntax::SyntaxKind::CallExpression: {
            const auto& call = static_cast<const syntax::CallExpressionSyntax&>(expression);
            return findKnownCallableReturnType({}, call.identifierToken.text, call.arguments.size(), false);
        }
        case syntax::SyntaxKind::MemberCallExpression: {
            const auto& call = static_cast<const syntax::MemberCallExpressionSyntax&>(expression);
            std::string recvType = call.receiver ? expressionType(*call.receiver) : std::string{};
            if (recvType.empty() && call.receiver &&
                call.receiver->kind() == syntax::SyntaxKind::NameExpression) {
                recvType = static_cast<const syntax::NameExpressionSyntax&>(
                    *call.receiver).identifierToken.text;
            }
            if (recvType == "string") {
                if (call.nameToken.text == "Length" || call.nameToken.text == "Count" ||
                    call.nameToken.text == "IndexOf" || call.nameToken.text == "LastIndexOf") return "int";
                if (call.nameToken.text == "Contains" || call.nameToken.text == "StartsWith" ||
                    call.nameToken.text == "EndsWith" || call.nameToken.text == "IsEmpty") return "bool";
                if (call.nameToken.text == "Substring" || call.nameToken.text == "Trim" ||
                    call.nameToken.text == "TrimStart" || call.nameToken.text == "TrimEnd" ||
                    call.nameToken.text == "Replace" || call.nameToken.text == "ToUpper" ||
                    call.nameToken.text == "ToLower") return "string";
                if (call.nameToken.text == "Split") return "string[]";
            }
            return findKnownCallableReturnType(
                recvType, call.nameToken.text, call.arguments.size(), true);
        }
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

    bool matchFormalTypeAgainstActual(
        const syntax::TypeSyntax& formal,
        const std::string& actual,
        const std::set<std::string>& typeParamSet,
        std::map<std::string, std::string>& inferred) const {
        if (actual.empty()) return false;
        if (formal.isArray()) {
            if (actual.size() <= 2 || actual.substr(actual.size() - 2) != "[]") {
                return false;
            }
            auto elemFormal = formal;
            elemFormal.openBracketToken.reset();
            elemFormal.closeBracketToken.reset();
            return matchFormalTypeAgainstActual(
                elemFormal, actual.substr(0, actual.size() - 2), typeParamSet, inferred);
        }
        if (formal.isGeneric()) {
            const auto formalSimple = stripCanonicalModulePrefix(formal.name.text);
            const auto actualSimple = stripCanonicalModulePrefix(actual);
            const auto instIt = instantiatedTypes_.find(actualSimple);
            if (instIt == instantiatedTypes_.end()) return false;
            if (instIt->second.first != formalSimple ||
                instIt->second.second.size() != formal.typeArguments.size()) {
                return false;
            }
            for (std::size_t i = 0; i < formal.typeArguments.size(); ++i) {
                if (!matchFormalTypeAgainstActual(
                        formal.typeArguments[i],
                        instIt->second.second[i],
                        typeParamSet,
                        inferred)) {
                    return false;
                }
            }
            return true;
        }
        if (typeParamSet.find(formal.name.text) != typeParamSet.end()) {
            const auto prev = inferred.find(formal.name.text);
            if (prev != inferred.end()) {
                if (prev->second == actual) return true;
                if (stripCanonicalModulePrefix(prev->second) ==
                    stripCanonicalModulePrefix(actual)) {
                    return true;
                }
                return false;
            }
            inferred[formal.name.text] = actual;
            return true;
        }
        return stripCanonicalModulePrefix(formal.name.text) ==
            stripCanonicalModulePrefix(actual);
    }

    const NativeGenericTemplate* findDelegateTemplateByName(
        const std::string& consumerModule,
        const std::string& rawName) const {
        const auto simple = stripCanonicalModulePrefix(rawName);
        const auto checkMod = [&](const std::string& modName) -> const NativeGenericTemplate* {
            const auto modIt = registry_.types.find(modName);
            if (modIt == registry_.types.end()) return nullptr;
            const auto found = modIt->second.find(simple);
            if (found != modIt->second.end() &&
                found->second.kind == NativeGenericTemplate::Kind::Delegate &&
                found->second.delegateSyntax) {
                return &found->second;
            }
            return nullptr;
        };
        if (const auto* own = checkMod(consumerModule)) return own;
        if (const auto modIt = modules_.find(consumerModule); modIt != modules_.end()) {
            for (const auto& imp : modIt->second.imports) {
                if (const auto* d = checkMod(imp)) return d;
            }
        }
        for (const auto& [modName, table] : registry_.types) {
            (void)table;
            if (const auto* d = checkMod(modName)) return d;
        }
        return nullptr;
    }

    std::optional<std::vector<std::string>> inferCallTypeArguments(
        const NativeGenericTemplate& declaration,
        const std::vector<const syntax::ExpressionSyntax*>& arguments,
        const std::string& consumerModule) {
        if (declaration.parameterTypeSyntaxes.size() != arguments.size()) {
            return std::nullopt;
        }
        std::set<std::string> typeParamSet(
            declaration.parameters.begin(), declaration.parameters.end());
        std::map<std::string, std::string> inferred;

        // Pass 1: infer from all non-lambda arguments
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (!arguments[index]) return std::nullopt;
            if (arguments[index]->kind() == syntax::SyntaxKind::LambdaExpression) {
                continue;
            }
            const auto actual = expressionType(*arguments[index]);
            if (actual.empty()) continue;
            if (!matchFormalTypeAgainstActual(
                    declaration.parameterTypeSyntaxes[index],
                    actual,
                    typeParamSet,
                    inferred)) {
                return std::nullopt;
            }
        }

        // Pass 2: infer from lambda arguments using already-inferred parameter types
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (!arguments[index] ||
                arguments[index]->kind() != syntax::SyntaxKind::LambdaExpression) {
                continue;
            }
            const auto& lambda = static_cast<const syntax::LambdaExpressionSyntax&>(
                *arguments[index]);
            const auto& formal = declaration.parameterTypeSyntaxes[index];
            if (!formal.isGeneric()) continue;
            const auto* delTmpl = findDelegateTemplateByName(
                declaration.moduleName.empty() ? consumerModule : declaration.moduleName,
                formal.name.text);
            if (!delTmpl || !delTmpl->delegateSyntax) continue;
            const auto& delSyn = *delTmpl->delegateSyntax;
            if (delSyn.parameters.size() != lambda.parameterTokens.size() ||
                delTmpl->parameters.size() != formal.typeArguments.size()) {
                continue;
            }

            // Map delegate type parameter -> method type expression using current 'inferred'
            std::map<std::string, syntax::TypeSyntax> delParamToMethodType;
            for (std::size_t ti = 0; ti < delTmpl->parameters.size(); ++ti) {
                delParamToMethodType[delTmpl->parameters[ti]] = formal.typeArguments[ti];
            }
            auto resolveDelFormalType = [&](const syntax::TypeSyntax& dt) -> std::string {
                if (!dt.isGeneric() && !dt.isArray()) {
                    if (const auto dm = delParamToMethodType.find(dt.name.text);
                        dm != delParamToMethodType.end()) {
                        const auto& mt = dm->second;
                        if (!mt.isGeneric() && !mt.isArray()) {
                            if (const auto inf = inferred.find(mt.name.text);
                                inf != inferred.end()) {
                                return inf->second;
                            }
                        }
                        return typeText(mt);
                    }
                }
                return typeText(dt);
            };

            const auto savedLocal = localTypes_;
            for (std::size_t pi = 0; pi < lambda.parameterTokens.size(); ++pi) {
                const auto pType = resolveDelFormalType(delSyn.parameters[pi].type);
                if (!pType.empty()) {
                    localTypes_[lambda.parameterTokens[pi].text] = pType;
                }
            }
            const auto bodyType = lambda.body ? expressionType(*lambda.body) : std::string{};
            localTypes_ = savedLocal;
            if (!bodyType.empty() && !delSyn.returnType.isGeneric() && !delSyn.returnType.isArray()) {
                if (const auto dm = delParamToMethodType.find(delSyn.returnType.name.text);
                    dm != delParamToMethodType.end()) {
                    if (!matchFormalTypeAgainstActual(
                            dm->second, bodyType, typeParamSet, inferred)) {
                        return std::nullopt;
                    }
                }
            }
        }

        std::vector<std::string> result;
        result.reserve(declaration.parameters.size());
        for (const auto& parameter : declaration.parameters) {
            const auto found = inferred.find(parameter);
            if (found == inferred.end()) return std::nullopt;
            result.push_back(found->second);
        }
        return result;
    }

    std::optional<std::vector<std::string>> inferArguments(
        const NativeGenericTemplate& declaration,
        const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& arguments,
        const std::string& consumerModule = {}) {
        std::vector<const syntax::ExpressionSyntax*> ptrs;
        ptrs.reserve(arguments.size());
        for (const auto& arg : arguments) ptrs.push_back(arg.get());
        return inferCallTypeArguments(declaration, ptrs, consumerModule);
    }

    static bool isPrimitiveOrKeywordTypeName(const std::string& name) noexcept {
        return name == "bool" || name == "byte" || name == "sbyte" ||
            name == "short" || name == "ushort" || name == "int" ||
            name == "uint" || name == "long" || name == "ulong" ||
            name == "float" || name == "double" || name == "char" ||
            name == "string" || name == "handle" || name == "void" ||
            name == "object" || name == "fixed" || name == "fixed16" ||
            name == "fixed32" || name == "fixed64" ||
            name == "vec2" || name == "vec3" || name == "vec4" || name == "quat";
    }

    bool moduleDeclaresTypeName(
        const std::string& modName,
        const std::string& typeName) const noexcept {
        const auto modIt = modules_.find(modName);
        if (modIt == modules_.end()) return false;
        for (const auto* u : modIt->second.units) {
            if (!u || !u->syntaxTree) continue;
            for (const auto& c : u->syntaxTree->classes) {
                if (c.typeParameters.empty() && c.identifierToken.text == typeName) return true;
            }
            for (const auto& s : u->syntaxTree->structs) {
                if (s.typeParameters.empty() && s.identifierToken.text == typeName) return true;
            }
            for (const auto& i : u->syntaxTree->interfaces) {
                if (i.typeParameters.empty() && i.identifierToken.text == typeName) return true;
            }
            for (const auto& e : u->syntaxTree->enums) {
                if (e.identifierToken.text == typeName) return true;
            }
            for (const auto& d : u->syntaxTree->delegates) {
                if (d.typeParameters.empty() && d.identifierToken.text == typeName) return true;
            }
        }
        return false;
    }

    HygienicSubstitutedType resolveHygienicArgument(
        const std::string& consumerModule,
        const std::string& rawArgument) const {
        HygienicSubstitutedType out;
        std::string base = rawArgument;
        while (base.size() >= 2 && base.substr(base.size() - 2) == "[]") {
            out.isArray = true;
            ++out.arrayRank;
            base.resize(base.size() - 2);
        }
        if (!base.empty() && base.back() == '?') {
            out.isNullable = true;
            base.pop_back();
        }
        const auto sep = base.find("::");
        if (sep != std::string::npos) {
            out.ownerModule = base.substr(0, sep);
            out.simpleBaseName = base.substr(sep + 2);
            out.canonicalBaseName = base;
            return out;
        }
        if (isPrimitiveOrKeywordTypeName(base)) {
            out.simpleBaseName = base;
            out.canonicalBaseName = base;
            return out;
        }
        out.simpleBaseName = base;
        if (moduleDeclaresTypeName(consumerModule, base)) {
            out.ownerModule = consumerModule;
            out.canonicalBaseName = consumerModule == "$global"
                ? base
                : (consumerModule + "::" + base);
            return out;
        }
        const auto consIt = modules_.find(consumerModule);
        if (consIt != modules_.end()) {
            for (const auto& imp : consIt->second.imports) {
                if (moduleDeclaresTypeName(imp, base)) {
                    out.ownerModule = imp;
                    out.canonicalBaseName = imp == "$global"
                        ? base
                        : (imp + "::" + base);
                    return out;
                }
            }
        }
        if (currentRewritingUnit_) {
            for (const auto& hyg : currentRewritingUnit_->unitHygienicModules) {
                if (moduleDeclaresTypeName(hyg, base)) {
                    out.ownerModule = hyg;
                    out.canonicalBaseName = hyg == "$global"
                        ? base
                        : (hyg + "::" + base);
                    return out;
                }
            }
        }
        for (const auto& hyg : currentMethodHygienicModules_) {
            if (moduleDeclaresTypeName(hyg, base)) {
                out.ownerModule = hyg;
                out.canonicalBaseName = hyg == "$global"
                    ? base
                    : (hyg + "::" + base);
                return out;
            }
        }
        out.canonicalBaseName = base;
        return out;
    }

    static bool isBuiltinOrMathTypeArg(const HygienicSubstitutedType& rArg) noexcept {
        if (isPrimitiveOrKeywordTypeName(rArg.simpleBaseName)) return true;
        if (rArg.ownerModule == "RealScript.Math") return true;
        const auto& s = rArg.simpleBaseName;
        return s == "Fixed" || s == "Fixed16" ||
            s == "Vec2" || s == "Vec3" || s == "Vec4" || s == "Quat";
    }

    bool isTemplateAccessibleFrom(
        const NativeGenericTemplate& decl,
        const std::string& consumerModule,
        const ParsedUnit* consumerUnit) const noexcept {
        if (decl.moduleName == consumerModule) return true;
        if (consumerUnit &&
            consumerUnit->unitHygienicModules.find(decl.moduleName) !=
                consumerUnit->unitHygienicModules.end()) {
            return true;
        }
        if (currentMethodHygienicModules_.find(decl.moduleName) !=
            currentMethodHygienicModules_.end()) {
            return true;
        }
        semantic::Accessibility acc = semantic::Accessibility::Public;
        if (decl.classSyntax) {
            acc = semantic::accessibilityFromModifiers(decl.classSyntax->modifiers);
        } else if (decl.structSyntax) {
            acc = semantic::accessibilityFromModifiers(decl.structSyntax->modifiers);
        } else if (decl.interfaceSyntax) {
            acc = semantic::accessibilityFromModifiers(decl.interfaceSyntax->modifiers);
        } else if (decl.delegateSyntax) {
            acc = semantic::accessibilityFromModifiers(decl.delegateSyntax->modifiers);
        } else if (decl.functionSyntax) {
            acc = semantic::accessibilityFromModifiers(decl.functionSyntax->modifiers);
        }
        return acc == semantic::Accessibility::Public;
    }

    bool isTypeArgAccessibleFrom(
        const HygienicSubstitutedType& rArg,
        const std::string& consumerModule,
        const ParsedUnit* consumerUnit) const noexcept {
        if (rArg.ownerModule.empty() ||
            rArg.ownerModule == consumerModule ||
            rArg.ownerModule == "$global" ||
            isBuiltinOrMathTypeArg(rArg)) {
            return true;
        }
        if (consumerUnit &&
            consumerUnit->unitHygienicModules.find(rArg.ownerModule) !=
                consumerUnit->unitHygienicModules.end()) {
            return true;
        }
        if (currentMethodHygienicModules_.find(rArg.ownerModule) !=
            currentMethodHygienicModules_.end()) {
            return true;
        }
        const auto modIt = modules_.find(rArg.ownerModule);
        if (modIt == modules_.end()) return true;
        for (const auto* u : modIt->second.units) {
            if (!u || !u->syntaxTree) continue;
            for (const auto& c : u->syntaxTree->classes) {
                if (c.typeParameters.empty() && c.identifierToken.text == rArg.simpleBaseName) {
                    return semantic::accessibilityFromModifiers(c.modifiers) == semantic::Accessibility::Public;
                }
            }
            for (const auto& s : u->syntaxTree->structs) {
                if (s.typeParameters.empty() && s.identifierToken.text == rArg.simpleBaseName) {
                    return semantic::accessibilityFromModifiers(s.modifiers) == semantic::Accessibility::Public;
                }
            }
            for (const auto& i : u->syntaxTree->interfaces) {
                if (i.typeParameters.empty() && i.identifierToken.text == rArg.simpleBaseName) {
                    return semantic::accessibilityFromModifiers(i.modifiers) == semantic::Accessibility::Public;
                }
            }
            for (const auto& e : u->syntaxTree->enums) {
                if (e.identifierToken.text == rArg.simpleBaseName) {
                    return semantic::accessibilityFromModifiers(e.modifiers) == semantic::Accessibility::Public;
                }
            }
            for (const auto& d : u->syntaxTree->delegates) {
                if (d.typeParameters.empty() && d.identifierToken.text == rArg.simpleBaseName) {
                    return semantic::accessibilityFromModifiers(d.modifiers) == semantic::Accessibility::Public;
                }
            }
        }
        return true;
    }

    static std::string formatMangleArgument(
        const HygienicSubstitutedType& resolved,
        const std::string& targetModule,
        bool isBuiltin) {
        const bool includeModulePrefix =
            !resolved.ownerModule.empty() &&
            resolved.ownerModule != targetModule &&
            resolved.ownerModule != "$global" &&
            !(isBuiltin && isBuiltinOrMathTypeArg(resolved));
        std::string text = includeModulePrefix
            ? (resolved.ownerModule + "::" + resolved.simpleBaseName)
            : resolved.simpleBaseName;
        if (resolved.isNullable) text += "?";
        if (resolved.isArray) text += "[]";
        return text;
    }

    std::string request(
        const std::string& consumerModule,
        const std::string& name,
        std::vector<std::string> arguments,
        bool function,
        text::TextSpan span,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        const auto simpleGenericName = stripCanonicalModulePrefix(name);
        const auto builtinArity = function
            ? std::size_t{0}
            : nativeBuiltinGenericArity(simpleGenericName);
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
            return simpleGenericName;
        }
        const auto expectedArity = declaration
            ? declaration->parameters.size()
            : builtinArity;
        if (arguments.size() != expectedArity) {
            result_.diagnostics.report(
                "RS8501",
                "generic arity mismatch for '" + simpleGenericName + "'",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return simpleGenericName;
        }

        std::vector<HygienicSubstitutedType> resolvedArgs;
        resolvedArgs.reserve(arguments.size());
        for (const auto& arg : arguments) {
            resolvedArgs.push_back(resolveHygienicArgument(consumerModule, arg));
            if (resolvedArgs.back().arrayRank > 1u) {
                result_.diagnostics.report(
                    "RS8511",
                    "nested array type argument '" + arg + "' (rank > 1) is not supported",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return simpleGenericName;
            }
        }

        if (!function && !declaration && simpleGenericName == "Nullable" &&
            resolvedArgs.size() == 1 && !resolvedArgs[0].isArray) {
            if (resolvedArgs[0].simpleBaseName.rfind("Nullable__", 0) == 0) {
                if (!resolvedArgs[0].ownerModule.empty() &&
                    resolvedArgs[0].ownerModule != consumerModule &&
                    resolvedArgs[0].ownerModule != "$global") {
                    modules_[consumerModule].hygienicModules.insert(resolvedArgs[0].ownerModule);
                    return resolvedArgs[0].canonicalBaseName;
                }
                return resolvedArgs[0].simpleBaseName;
            }
            if (resolvedArgs[0].isNullable) {
                resolvedArgs[0].isNullable = false;
            }
        }

        if (declaration && !isTemplateAccessibleFrom(
                *declaration, consumerModule, currentRewritingUnit_)) {
            result_.diagnostics.report(
                "RS2534",
                "generic declaration '" + declaration->name + "' is inaccessible",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return simpleGenericName;
        }
        for (std::size_t idx = 0; idx < resolvedArgs.size(); ++idx) {
            if (!isTypeArgAccessibleFrom(resolvedArgs[idx], consumerModule, currentRewritingUnit_)) {
                result_.diagnostics.report(
                    "RS2534",
                    "type argument '" + arguments[idx] + "' is inaccessible",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return simpleGenericName;
            }
        }

        if (declaration && !validateConstraints(
                *declaration, resolvedArgs, arguments, span, sourceName, consumerModule)) {
            return simpleGenericName;
        }

        auto isMathStructTypeArg = [&](const HygienicSubstitutedType& rArg) noexcept {
            const auto& s = rArg.simpleBaseName;
            return s == "Fixed" || s == "Fixed16" ||
                s == "Vec2" || s == "Vec3" || s == "Vec4" || s == "Quat";
        };
        auto isCanonicalBuiltinTypeArg = [&](const HygienicSubstitutedType& rArg) noexcept {
            return isPrimitiveOrKeywordTypeName(rArg.simpleBaseName) ||
                isMathStructTypeArg(rArg) ||
                rArg.simpleBaseName == "Awaitable" ||
                rArg.simpleBaseName == "CancellationToken";
        };
        auto resolveAwaitableOwnerModule = [&](const HygienicSubstitutedType& rArg) -> std::string {
            if (isCanonicalBuiltinTypeArg(rArg) &&
                modules_.find("RealScript.Async") != modules_.end()) {
                if (isMathStructTypeArg(rArg) &&
                    modules_.find("RealScript.Math") != modules_.end()) {
                    modules_["RealScript.Async"].imports.insert("RealScript.Math");
                }
                return "RealScript.Async";
            }
            if (!rArg.ownerModule.empty() && modules_.find(rArg.ownerModule) != modules_.end()) {
                if (modules_.find("RealScript.Async") != modules_.end()) {
                    modules_[rArg.ownerModule].imports.insert("RealScript.Async");
                }
                return rArg.ownerModule;
            }
            return consumerModule;
        };

        const bool isBuiltin = (declaration == nullptr);
        const auto targetModule = declaration
            ? declaration->moduleName
            : ((simpleGenericName == "Awaitable" && resolvedArgs.size() == 1)
                   ? resolveAwaitableOwnerModule(resolvedArgs[0])
                   : ((simpleGenericName == "ResourceHandle" &&
                       modules_.find("RealScript.Resource") != modules_.end() &&
                       !resolvedArgs.empty() &&
                       !resolvedArgs[0].ownerModule.empty())
                          ? resolvedArgs[0].ownerModule
                          : consumerModule));

        if (simpleGenericName == "ResourceHandle" &&
            modules_.find("RealScript.Resource") != modules_.end()) {
            modules_[targetModule].imports.insert("RealScript.Resource");
        }

        std::vector<std::string> mangleArgs;
        mangleArgs.reserve(resolvedArgs.size());
        for (const auto& rArg : resolvedArgs) {
            mangleArgs.push_back(formatMangleArgument(rArg, targetModule, isBuiltin));
        }

        const auto baseTemplateName = declaration ? declaration->name : simpleGenericName;
        const auto generatedName = nativeGenericGeneratedName(baseTemplateName, mangleArgs);
        const auto returnReferenceName = (!function && targetModule != consumerModule && targetModule != "$global")
            ? (targetModule + "::" + generatedName)
            : generatedName;

        if (targetModule != consumerModule &&
            (!function || (declaration && declaration->isExtension))) {
            modules_[consumerModule].hygienicModules.insert(targetModule);
        }
        if (!function) {
            std::vector<std::string> rawResolvedArgNames;
            rawResolvedArgNames.reserve(resolvedArgs.size());
            for (const auto& rArg : resolvedArgs) {
                std::string argName = rArg.canonicalBaseName.empty()
                    ? rArg.simpleBaseName
                    : rArg.canonicalBaseName;
                if (rArg.isNullable) argName = "Nullable__" + stripCanonicalModulePrefix(argName);
                if (rArg.isArray) argName += "[]";
                rawResolvedArgNames.push_back(std::move(argName));
            }
            instantiatedTypes_[generatedName] = {baseTemplateName, std::move(rawResolvedArgNames)};
        }
        std::set<std::string> specUnitHygienic;
        if (declaration && !declaration->moduleName.empty()) {
            specUnitHygienic.insert(declaration->moduleName);
        } else if (simpleGenericName == "ResourceHandle") {
            specUnitHygienic.insert("RealScript.Resource");
        } else if (simpleGenericName == "Awaitable") {
            specUnitHygienic.insert("RealScript.Async");
        }
        for (const auto& rArg : resolvedArgs) {
            if (!rArg.ownerModule.empty() && rArg.ownerModule != targetModule) {
                modules_[targetModule].hygienicModules.insert(rArg.ownerModule);
                specUnitHygienic.insert(rArg.ownerModule);
            }
        }
        for (const auto& hm : specUnitHygienic) {
            if (!hm.empty() && hm != targetModule) {
                modules_[targetModule].hygienicModules.insert(hm);
            }
        }

        const auto key = targetModule + "::" +
            (function ? "function:" : "type:") + generatedName;
        if (!specializations_.insert(key).second) {
            return returnReferenceName;
        }

        if (currentGenericDepth_ >= 32u) {
            result_.diagnostics.report(
                "RS8512",
                "generic specialization depth limit (32) exceeded while instantiating '" +
                    generatedName + "'",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return returnReferenceName;
        }

        if (declaration) {
            if (!declaration->unit || !declaration->unit->source) {
                result_.diagnostics.report(
                    "RS8510",
                    "generic specialization '" + generatedName + "' could not be generated",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return returnReferenceName;
            }

            ast_monomorphizer::SubstitutionMap subs;
            for (std::size_t idx = 0; idx < declaration->parameters.size(); ++idx) {
                auto subEntry = resolvedArgs[idx];
                if (subEntry.ownerModule == targetModule) {
                    subEntry.canonicalBaseName = subEntry.simpleBaseName;
                }
                subs.emplace(declaration->parameters[idx], std::move(subEntry));
            }

            auto unit = std::make_unique<ParsedUnit>();
            unit->source = declaration->unit->source;
            unit->syntaxTree = std::make_shared<syntax::CompilationUnitSyntax>();
            unit->syntaxTree->endOfFileToken = declaration->unit->syntaxTree->endOfFileToken;
            bool hasRemainingGenerics = false;
            ast_monomorphizer::arrayRankOverflowFlag() = false;

            switch (declaration->kind) {
            case NativeGenericTemplate::Kind::Class:
                if (declaration->classSyntax) {
                    auto cloned = syntax::cloneClassDeclarationSyntax(*declaration->classSyntax);
                    ast_monomorphizer::substituteTypeLikeDeclaration(
                        cloned, declaration->name, generatedName, subs, hasRemainingGenerics);
                    unit->syntaxTree->classes.push_back(std::move(cloned));
                }
                break;
            case NativeGenericTemplate::Kind::Struct:
                if (declaration->structSyntax) {
                    auto cloned = syntax::cloneStructDeclarationSyntax(*declaration->structSyntax);
                    ast_monomorphizer::substituteTypeLikeDeclaration(
                        cloned, declaration->name, generatedName, subs, hasRemainingGenerics);
                    unit->syntaxTree->structs.push_back(std::move(cloned));
                }
                break;
            case NativeGenericTemplate::Kind::Interface:
                if (declaration->interfaceSyntax) {
                    auto cloned = syntax::cloneInterfaceDeclarationSyntax(*declaration->interfaceSyntax);
                    cloned.identifierToken.text = generatedName;
                    cloned.typeParameterLessToken.reset();
                    cloned.typeParameters.clear();
                    cloned.typeParameterCommaTokens.clear();
                    cloned.typeParameterGreaterToken.reset();
                    cloned.constraints.clear();
                    std::set<std::string> ifaceMethodNames;
                    for (const auto& m : cloned.methods) {
                        ifaceMethodNames.insert(m.identifierToken.text);
                    }
                    for (auto& m : cloned.methods) {
                        ast_monomorphizer::substituteType(m.returnType, subs, hasRemainingGenerics);
                        auto shadowed = ifaceMethodNames;
                        for (auto& p : m.parameters) {
                            ast_monomorphizer::substituteType(p.type, subs, hasRemainingGenerics);
                            if (p.defaultValue) {
                                ast_monomorphizer::substituteExpression(
                                    *p.defaultValue, subs, shadowed, hasRemainingGenerics);
                            }
                            shadowed.insert(p.identifierToken.text);
                        }
                        if (m.hasBody()) {
                            ast_monomorphizer::substituteStatement(
                                *m.body, subs, shadowed, hasRemainingGenerics);
                        }
                    }
                    unit->syntaxTree->interfaces.push_back(std::move(cloned));
                }
                break;
            case NativeGenericTemplate::Kind::Delegate:
                if (declaration->delegateSyntax) {
                    auto cloned = syntax::cloneDelegateDeclarationSyntax(*declaration->delegateSyntax);
                    cloned.identifierToken.text = generatedName;
                    cloned.typeParameterLessToken.reset();
                    cloned.typeParameters.clear();
                    cloned.typeParameterCommaTokens.clear();
                    cloned.typeParameterGreaterToken.reset();
                    cloned.constraints.clear();
                    ast_monomorphizer::substituteType(cloned.returnType, subs, hasRemainingGenerics);
                    for (auto& p : cloned.parameters) {
                        ast_monomorphizer::substituteType(p.type, subs, hasRemainingGenerics);
                    }
                    unit->syntaxTree->delegates.push_back(std::move(cloned));
                }
                break;
            case NativeGenericTemplate::Kind::Function:
                if (declaration->functionSyntax) {
                    auto cloned = syntax::cloneFunctionDeclarationSyntax(*declaration->functionSyntax);
                    cloned.identifierToken.text = generatedName;
                    cloned.typeParameterLessToken.reset();
                    cloned.typeParameters.clear();
                    cloned.typeParameterCommaTokens.clear();
                    cloned.typeParameterGreaterToken.reset();
                    cloned.constraints.clear();
                    std::set<std::string> emptyMembers;
                    ast_monomorphizer::substituteFunctionDeclaration(
                        cloned, subs, emptyMembers, hasRemainingGenerics);
                    auto retCopy = cloned.returnType;
                    if (retCopy.isGeneric()) {
                        const auto savedLocal = localTypes_;
                        rewriteType(retCopy, targetModule, declaration->sourceName, queue);
                        localTypes_ = savedLocal;
                    }
                    specializedReturnTypes_[generatedName] = typeText(retCopy);
                    unit->syntaxTree->functions.push_back(std::move(cloned));
                }
                break;
            }
            if (ast_monomorphizer::arrayRankOverflowFlag()) {
                ast_monomorphizer::arrayRankOverflowFlag() = false;
                result_.diagnostics.report(
                    "RS8511",
                    "generic specialization '" + generatedName +
                        "' produces nested array type (rank > 1), which is not supported",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return returnReferenceName;
            }

            unit->hasGenericSyntax = hasRemainingGenerics;
            unit->moduleName = targetModule;
            unit->deterministic = declaration->unit->deterministic;
            unit->isGenericSpecialization = true;
            unit->genericDepth = currentGenericDepth_ + 1u;
            unit->unitHygienicModules = specUnitHygienic;
            unit->sourceFingerprint = combineFingerprint(
                declaration->unit->sourceFingerprint,
                stableFingerprint("ast-spec:" + key));

            auto* pointer = unit.get();
            if (!pointer->syntaxTree->classes.empty()) {
                registerDynamicMemberTemplatesFromType(
                    targetModule, pointer, pointer->syntaxTree->classes.front());
            }
            if (!pointer->syntaxTree->structs.empty()) {
                registerDynamicMemberTemplatesFromStruct(
                    targetModule, pointer, pointer->syntaxTree->structs.front());
            }

            auto& target = modules_[targetModule];
            target.name = targetModule;
            target.units.push_back(pointer);
            units_.push_back(std::move(unit));
            queue.push_back(pointer);
            result_.nativeGenericInstantiations.push_back(
                LanguageGenericInstantiation{baseTemplateName, mangleArgs, generatedName});
            return returnReferenceName;
        }

        const auto body = nativeBuiltinGenericSource(simpleGenericName, mangleArgs, generatedName);
        if (body.empty()) {
            result_.diagnostics.report(
                "RS8510",
                "generic specialization '" + generatedName +
                    "' could not be generated",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return returnReferenceName;
        }

        std::ostringstream generated;
        generated << "module " << targetModule << ";\n" << body << '\n';
        const auto path = "$generated/generics/" +
            nativeGenericSanitize(targetModule) + "/" +
            generatedName + ".rscript";
        auto unit = std::make_unique<ParsedUnit>();
        unit->source = std::make_shared<text::SourceText>(
            generated.str(), path);
        diagnostics::DiagnosticBag parseDiagnostics;
        syntax::Parser parser(*unit->source, parseDiagnostics);
        unit->syntaxTree = std::make_shared<syntax::CompilationUnitSyntax>(
            parser.parseCompilationUnit());

        ast_monomorphizer::SubstitutionMap builtinSubs;
        for (std::size_t idx = 0; idx < resolvedArgs.size(); ++idx) {
            auto subEntry = resolvedArgs[idx];
            if (subEntry.ownerModule == targetModule) {
                subEntry.canonicalBaseName = subEntry.simpleBaseName;
            }
            builtinSubs.emplace("__RsBuiltinTypeArg" + std::to_string(idx), std::move(subEntry));
        }
        bool builtinHasGenerics = (generated.str().find('<') != std::string::npos);
        ast_monomorphizer::arrayRankOverflowFlag() = false;
        ast_monomorphizer::substituteCompilationUnit(
            *unit->syntaxTree, builtinSubs, builtinHasGenerics);
        if (ast_monomorphizer::arrayRankOverflowFlag()) {
            ast_monomorphizer::arrayRankOverflowFlag() = false;
            result_.diagnostics.report(
                "RS8511",
                "generic specialization '" + generatedName +
                    "' produces nested array type (rank > 1), which is not supported",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return returnReferenceName;
        }

        unit->hasGenericSyntax = builtinHasGenerics;
        unit->moduleName = targetModule;
        unit->isGenericSpecialization = true;
        unit->genericDepth = currentGenericDepth_ + 1u;
        unit->unitHygienicModules = specUnitHygienic;
        unit->sourceFingerprint = stableFingerprint(
            path + "\n" + key + "\n" + generated.str());
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
            LanguageGenericInstantiation{simpleGenericName, mangleArgs, generatedName});
        return returnReferenceName;
    }

    struct ConcreteTypeFacts {
        bool known = false;
        bool valueType = false;
        bool referenceType = false;
        bool defaultConstructor = false;
        std::set<std::string> supertypes;
    };

    void collectSupertypes(
        const std::string& ownerModule,
        const std::string& simpleTypeName,
        std::set<std::string>& outSupertypes,
        std::set<std::string>& visited) const {
        const std::string visitKey = ownerModule.empty()
            ? simpleTypeName
            : (ownerModule + "::" + simpleTypeName);
        if (simpleTypeName.empty() || !visited.insert(visitKey).second) return;
        outSupertypes.insert(simpleTypeName);
        if (!ownerModule.empty()) {
            outSupertypes.insert(ownerModule + "::" + simpleTypeName);
        }
        auto scanUnit = [&](const ParsedUnit* unit) {
            if (!unit || !unit->syntaxTree) return;
            for (const auto& value : unit->syntaxTree->classes) {
                if (value.identifierToken.text != simpleTypeName) continue;
                for (const auto& iface : value.interfaces) {
                    const auto parentRaw = nativeGenericTypeText(iface);
                    const auto resolvedParent = resolveHygienicArgument(unit->moduleName, parentRaw);
                    outSupertypes.insert(parentRaw);
                    outSupertypes.insert(resolvedParent.simpleBaseName);
                    if (!resolvedParent.ownerModule.empty()) {
                        outSupertypes.insert(resolvedParent.canonicalBaseName);
                    }
                    if (iface.isGeneric()) {
                        auto ifaceCopy = iface;
                        const auto savedLocal = localTypes_;
                        std::vector<ParsedUnit*> dummyQueue;
                        const_cast<NativeGenericSpecializer*>(this)->rewriteType(
                            ifaceCopy, unit->moduleName, unit->source->name(), dummyQueue);
                        for (std::size_t qIdx = 0; qIdx < dummyQueue.size(); ++qIdx) {
                            auto* du = dummyQueue[qIdx];
                            if (!du || !const_cast<NativeGenericSpecializer*>(this)->processed_.insert(du).second) continue;
                            const_cast<NativeGenericSpecializer*>(this)->rewriteUnit(*du, dummyQueue);
                        }
                        const_cast<NativeGenericSpecializer*>(this)->localTypes_ = savedLocal;
                        const auto specText = nativeGenericTypeText(ifaceCopy);
                        const auto resolvedSpec = resolveHygienicArgument(unit->moduleName, specText);
                        outSupertypes.insert(specText);
                        outSupertypes.insert(resolvedSpec.simpleBaseName);
                        if (!resolvedSpec.ownerModule.empty()) {
                            outSupertypes.insert(resolvedSpec.canonicalBaseName);
                        }
                    }
                    collectSupertypes(
                        resolvedParent.ownerModule,
                        resolvedParent.simpleBaseName,
                        outSupertypes,
                        visited);
                }
            }
            for (const auto& value : unit->syntaxTree->structs) {
                if (value.identifierToken.text != simpleTypeName) continue;
                for (const auto& iface : value.interfaces) {
                    const auto parentRaw = nativeGenericTypeText(iface);
                    const auto resolvedParent = resolveHygienicArgument(unit->moduleName, parentRaw);
                    outSupertypes.insert(parentRaw);
                    outSupertypes.insert(resolvedParent.simpleBaseName);
                    if (!resolvedParent.ownerModule.empty()) {
                        outSupertypes.insert(resolvedParent.canonicalBaseName);
                    }
                    if (iface.isGeneric()) {
                        auto ifaceCopy = iface;
                        const auto savedLocal = localTypes_;
                        std::vector<ParsedUnit*> dummyQueue;
                        const_cast<NativeGenericSpecializer*>(this)->rewriteType(
                            ifaceCopy, unit->moduleName, unit->source->name(), dummyQueue);
                        for (std::size_t qIdx = 0; qIdx < dummyQueue.size(); ++qIdx) {
                            auto* du = dummyQueue[qIdx];
                            if (!du || !const_cast<NativeGenericSpecializer*>(this)->processed_.insert(du).second) continue;
                            const_cast<NativeGenericSpecializer*>(this)->rewriteUnit(*du, dummyQueue);
                        }
                        const_cast<NativeGenericSpecializer*>(this)->localTypes_ = savedLocal;
                        const auto specText = nativeGenericTypeText(ifaceCopy);
                        const auto resolvedSpec = resolveHygienicArgument(unit->moduleName, specText);
                        outSupertypes.insert(specText);
                        outSupertypes.insert(resolvedSpec.simpleBaseName);
                        if (!resolvedSpec.ownerModule.empty()) {
                            outSupertypes.insert(resolvedSpec.canonicalBaseName);
                        }
                    }
                    collectSupertypes(
                        resolvedParent.ownerModule,
                        resolvedParent.simpleBaseName,
                        outSupertypes,
                        visited);
                }
            }
        };
        if (!ownerModule.empty()) {
            const auto modIt = modules_.find(ownerModule);
            if (modIt != modules_.end()) {
                for (const auto* u : modIt->second.units) scanUnit(u);
                return;
            }
        }
        for (const auto& unit : units_) scanUnit(unit.get());
    }

    ConcreteTypeFacts concreteTypeFacts(const HygienicSubstitutedType& arg) const {
        if (arg.isArray) {
            return {true, false, true, false, {arg.simpleBaseName + "[]", "object"}};
        }
        if (arg.isNullable) {
            return {true, true, false, true, {"Nullable", "object"}};
        }
        const auto& name = arg.simpleBaseName;
        static const std::set<std::string> primitiveValues = {
            "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
            "long", "ulong", "float", "double", "char", "fixed", "fixed16",
            "fixed32", "fixed64", "vec2", "vec3", "vec4", "quat", "handle"};
        if (primitiveValues.find(name) != primitiveValues.end()) {
            return {true, true, false, true, {name}};
        }
        if (name == "string" || name == "object") {
            return {true, false, true, false, {name, "object"}};
        }
        std::set<std::string> supertypes;
        std::set<std::string> visited;
        collectSupertypes(arg.ownerModule, name, supertypes, visited);
        supertypes.insert("object");

        auto inspectUnit = [&](const ParsedUnit* unit) -> std::optional<ConcreteTypeFacts> {
            if (!unit || !unit->syntaxTree) return std::nullopt;
            for (const auto& value : unit->syntaxTree->structs) {
                if (value.typeParameters.empty() && value.identifierToken.text == name) {
                    return ConcreteTypeFacts{true, true, false, true, supertypes};
                }
            }
            for (const auto& value : unit->syntaxTree->enums) {
                if (value.identifierToken.text == name) {
                    return ConcreteTypeFacts{true, true, false, true, supertypes};
                }
            }
            for (const auto& value : unit->syntaxTree->classes) {
                if (!value.typeParameters.empty() || value.identifierToken.text != name) continue;
                const bool isAbstractOrStatic =
                    semantic::hasModifier(value.modifiers, syntax::SyntaxKind::AbstractKeyword) ||
                    semantic::hasModifier(value.modifiers, syntax::SyntaxKind::StaticKeyword);
                bool hasInstanceConstructor = false;
                bool defaultConstructor = false;
                if (!isAbstractOrStatic) {
                    for (const auto& constructor : value.constructors) {
                        if (semantic::hasModifier(
                                constructor.modifiers,
                                syntax::SyntaxKind::StaticKeyword)) {
                            continue;
                        }
                        hasInstanceConstructor = true;
                        if (constructor.parameters.empty() &&
                            semantic::accessibilityFromModifiers(constructor.modifiers) ==
                                semantic::Accessibility::Public) {
                            defaultConstructor = true;
                        }
                    }
                    if (!hasInstanceConstructor) defaultConstructor = true;
                }
                return ConcreteTypeFacts{true, false, true, defaultConstructor, supertypes};
            }
            for (const auto& value : unit->syntaxTree->interfaces) {
                if (value.typeParameters.empty() && value.identifierToken.text == name) {
                    return ConcreteTypeFacts{true, false, true, false, supertypes};
                }
            }
            for (const auto& value : unit->syntaxTree->delegates) {
                if (value.typeParameters.empty() && value.identifierToken.text == name) {
                    return ConcreteTypeFacts{true, false, true, false, supertypes};
                }
            }
            return std::nullopt;
        };

        if (!arg.ownerModule.empty()) {
            const auto modIt = modules_.find(arg.ownerModule);
            if (modIt != modules_.end()) {
                for (const auto* u : modIt->second.units) {
                    if (auto found = inspectUnit(u)) return *found;
                }
            }
        }
        for (const auto& unit : units_) {
            if (auto found = inspectUnit(unit.get())) return *found;
        }
        return {};
    }

    std::string resolveConstraintTypeName(
        const std::string& rawConstraint,
        const NativeGenericTemplate& declaration,
        const std::vector<HygienicSubstitutedType>& resolvedArgs,
        const std::string& consumerModule,
        text::TextSpan span,
        const std::string& sourceName,
        std::string* outSubstitutedText = nullptr) {
        const std::string constraintModule =
            declaration.moduleName.empty() ? consumerModule : declaration.moduleName;
        std::map<std::string, std::string> substitutions;
        for (std::size_t i = 0; i < declaration.parameters.size() && i < resolvedArgs.size(); ++i) {
            const auto& rArg = resolvedArgs[i];
            std::string subText = (!rArg.ownerModule.empty() &&
                                   rArg.ownerModule != constraintModule &&
                                   rArg.ownerModule != "$global")
                ? (rArg.ownerModule + "::" + rArg.simpleBaseName)
                : rArg.simpleBaseName;
            if (rArg.isNullable) subText += "?";
            if (rArg.isArray) subText += "[]";
            substitutions.emplace(declaration.parameters[i], std::move(subText));
        }
        (void)span;
        text::SourceText source("class __RsConstraintHolder { " + rawConstraint + " f; }", sourceName);
        diagnostics::DiagnosticBag ignored;
        syntax::Parser parser(source, ignored);
        auto unit = parser.parseCompilationUnit();
        if (unit.classes.empty() || unit.classes.front().fields.empty()) {
            const auto resolvedFallback = resolveHygienicArgument(constraintModule, rawConstraint);
            const auto out = !resolvedFallback.canonicalBaseName.empty()
                ? resolvedFallback.canonicalBaseName
                : rawConstraint;
            if (outSubstitutedText) *outSubstitutedText = out;
            return out;
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
            rewriteType(parsedType, constraintModule, sourceName, dummyQueue);
            for (std::size_t qIdx = 0; qIdx < dummyQueue.size(); ++qIdx) {
                auto* u = dummyQueue[qIdx];
                if (!u || !processed_.insert(u).second) continue;
                rewriteUnit(*u, dummyQueue);
            }
            localTypes_ = savedLocalTypes;
        }
        const auto rewrittenText = nativeGenericTypeText(parsedType);
        const auto resolvedConstraint = resolveHygienicArgument(constraintModule, rewrittenText);
        if (!resolvedConstraint.ownerModule.empty() &&
            resolvedConstraint.ownerModule != "$global") {
            std::string canonical = resolvedConstraint.canonicalBaseName;
            if (resolvedConstraint.isNullable) canonical += "?";
            if (resolvedConstraint.isArray) canonical += "[]";
            return canonical;
        }
        return rewrittenText;
    }

    bool validateConstraints(
        const NativeGenericTemplate& declaration,
        const std::vector<HygienicSubstitutedType>& resolvedArgs,
        const std::vector<std::string>& rawArguments,
        text::TextSpan span,
        const std::string& sourceName,
        const std::string& consumerModule) {
        bool valid = true;
        for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
            const auto found = declaration.constraints.find(
                declaration.parameters[index]);
            if (found == declaration.constraints.end()) continue;
            const auto facts = concreteTypeFacts(resolvedArgs[index]);
            for (const auto& constraint : found->second) {
                bool satisfied = true;
                if (constraint == "class") {
                    satisfied = facts.referenceType;
                } else if (constraint == "struct") {
                    satisfied = facts.valueType;
                } else if (constraint == "new()") {
                    satisfied = facts.defaultConstructor;
                } else {
                    std::string substitutedType;
                    const auto expectedType = resolveConstraintTypeName(
                        constraint, declaration, resolvedArgs, consumerModule, span, sourceName, &substitutedType);
                    const bool hasQualifiedExpected =
                        expectedType.find("::") != std::string::npos;
                    if (hasQualifiedExpected) {
                        satisfied = facts.known &&
                            facts.supertypes.find(expectedType) != facts.supertypes.end();
                    } else {
                        const auto simpleExpected = stripCanonicalModulePrefix(expectedType);
                        satisfied = facts.known &&
                            (facts.supertypes.find(expectedType) != facts.supertypes.end() ||
                             facts.supertypes.find(simpleExpected) != facts.supertypes.end() ||
                             (!substitutedType.empty() &&
                              facts.supertypes.find(substitutedType) != facts.supertypes.end()) ||
                             facts.supertypes.find(constraint) != facts.supertypes.end());
                    }
                }
                if (satisfied) continue;
                result_.diagnostics.report(
                    "RS8530",
                    "type argument '" + rawArguments[index] +
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
        if (!isTemplateAccessibleFrom(declaration, consumerModule, currentRewritingUnit_)) {
            result_.diagnostics.report(
                "RS2534",
                "generic member '" + declaration.name + "' is inaccessible",
                span,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[consumerModule].invalid = true;
            return declaration.name;
        }
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
        std::vector<HygienicSubstitutedType> resolvedArgs;
        resolvedArgs.reserve(arguments.size());
        for (const auto& arg : arguments) {
            resolvedArgs.push_back(resolveHygienicArgument(consumerModule, arg));
            if (resolvedArgs.back().arrayRank > 1u) {
                result_.diagnostics.report(
                    "RS8511",
                    "nested array type argument '" + arg + "' (rank > 1) is not supported",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return declaration.name;
            }
        }
        for (std::size_t idx = 0; idx < resolvedArgs.size(); ++idx) {
            if (!isTypeArgAccessibleFrom(resolvedArgs[idx], consumerModule, currentRewritingUnit_)) {
                result_.diagnostics.report(
                    "RS2534",
                    "type argument '" + arguments[idx] + "' is inaccessible",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return declaration.name;
            }
        }
        if (!validateConstraints(
                declaration, resolvedArgs, arguments, span, sourceName, consumerModule)) {
            return declaration.name;
        }
        std::vector<std::string> mangleArgs;
        mangleArgs.reserve(resolvedArgs.size());
        for (const auto& rArg : resolvedArgs) {
            mangleArgs.push_back(formatMangleArgument(rArg, declaration.moduleName, false));
            if (!rArg.ownerModule.empty() && rArg.ownerModule != declaration.moduleName) {
                modules_[declaration.moduleName].hygienicModules.insert(rArg.ownerModule);
            }
        }
        if (!declaration.moduleName.empty() && declaration.moduleName != consumerModule) {
            modules_[consumerModule].hygienicModules.insert(declaration.moduleName);
        }
        const auto generated = nativeGenericGeneratedName(
            declaration.name, mangleArgs);
        if (declaration.returnTypeSyntax) {
            ast_monomorphizer::SubstitutionMap retSubs;
            for (std::size_t idx = 0;
                 idx < declaration.parameters.size() && idx < resolvedArgs.size();
                 ++idx) {
                auto rArg = resolvedArgs[idx];
                if (rArg.ownerModule == declaration.moduleName) {
                    rArg.canonicalBaseName = rArg.simpleBaseName;
                }
                retSubs.emplace(declaration.parameters[idx], std::move(rArg));
            }
            auto retCopy = *declaration.returnTypeSyntax;
            bool dummyHasGen = false;
            ast_monomorphizer::substituteType(retCopy, retSubs, dummyHasGen);
            if (retCopy.isGeneric()) {
                const auto savedLocal = localTypes_;
                std::vector<ParsedUnit*> retQueue;
                rewriteType(retCopy, declaration.moduleName, declaration.sourceName, retQueue);
                for (std::size_t qi = 0; qi < retQueue.size(); ++qi) {
                    auto* u = retQueue[qi];
                    if (!u || !processed_.insert(u).second) continue;
                    rewriteUnit(*u, retQueue);
                }
                localTypes_ = savedLocal;
            }
            specializedReturnTypes_[generated] = typeText(retCopy);
        }
        const auto key = declaration.moduleName + "::member:" +
            declaration.ownerName + "::" + generated;
        if (specializations_.insert(key).second) {
            if (currentGenericDepth_ >= 32u) {
                result_.diagnostics.report(
                    "RS8512",
                    "generic specialization depth limit (32) exceeded while instantiating member '" +
                        generated + "'",
                    span,
                    diagnostics::DiagnosticSeverity::Error,
                    sourceName);
                modules_[consumerModule].invalid = true;
                return generated;
            }
            pendingMembers_.push_back(PendingNativeGenericMember{
                &declaration,
                consumerModule,
                std::move(resolvedArgs),
                generated,
                span,
                sourceName,
                currentGenericDepth_ + 1u});
            result_.nativeGenericInstantiations.push_back(
                LanguageGenericInstantiation{
                    declaration.ownerName + "." + declaration.name,
                    mangleArgs,
                    generated});
        }
        return generated;
    }

    void materializeMember(
        const PendingNativeGenericMember& pending,
        std::vector<ParsedUnit*>& queue) {
        if (!pending.declaration || !pending.declaration->unit ||
            !pending.declaration->functionSyntax) {
            return;
        }
        ast_monomorphizer::SubstitutionMap subs;
        for (std::size_t idx = 0;
             idx < pending.declaration->parameters.size() && idx < pending.resolvedArgs.size();
             ++idx) {
            auto rArg = pending.resolvedArgs[idx];
            if (rArg.ownerModule == pending.declaration->moduleName) {
                rArg.canonicalBaseName = rArg.simpleBaseName;
            }
            subs.emplace(pending.declaration->parameters[idx], std::move(rArg));
        }
        auto method = syntax::cloneFunctionDeclarationSyntax(*pending.declaration->functionSyntax);
        method.identifierToken.text = pending.generatedName;
        method.typeParameterLessToken.reset();
        method.typeParameters.clear();
        method.typeParameterCommaTokens.clear();
        method.typeParameterGreaterToken.reset();
        method.constraints.clear();
        if (!pending.declaration->moduleName.empty()) {
            method.hygienicModules.push_back(pending.declaration->moduleName);
        }
        for (const auto& [paramName, rArg] : subs) {
            (void)paramName;
            if (!rArg.ownerModule.empty()) {
                method.hygienicModules.push_back(rArg.ownerModule);
            }
        }

        auto* ownerUnit = const_cast<ParsedUnit*>(pending.declaration->unit);
        detachSyntaxTree(*ownerUnit);
        ownerUnit->sourceFingerprint = combineFingerprint(
            ownerUnit->sourceFingerprint,
            stableFingerprint("member-spec:" + pending.generatedName));

        const auto* prevUnit = currentRewritingUnit_;
        const auto prevDepth = currentGenericDepth_;
        const auto prevLocalTypes = localTypes_;
        const auto prevMethodHyg = currentMethodHygienicModules_;
        currentRewritingUnit_ = ownerUnit;
        currentGenericDepth_ = pending.genericDepth;
        currentMethodHygienicModules_.clear();
        currentMethodHygienicModules_.insert(
            method.hygienicModules.begin(), method.hygienicModules.end());
        struct ScopedRestore {
            NativeGenericSpecializer& self;
            const ParsedUnit* prevUnit;
            std::uint32_t prevDepth;
            std::map<std::string, std::string> prevLocalTypes;
            std::set<std::string> prevMethodHyg;
            ~ScopedRestore() {
                self.currentRewritingUnit_ = prevUnit;
                self.currentGenericDepth_ = prevDepth;
                self.localTypes_ = std::move(prevLocalTypes);
                self.currentMethodHygienicModules_ = std::move(prevMethodHyg);
            }
        } restore{*this, prevUnit, prevDepth, prevLocalTypes, prevMethodHyg};

        auto rewriteSpecializedMemberOn = [&](auto& declaration) -> bool {
            if (declaration.identifierToken.text != pending.declaration->ownerName) {
                return false;
            }
            std::set<std::string> memberNames;
            localTypes_.clear();
            for (const auto& f : declaration.fields) {
                memberNames.insert(f.identifierToken.text);
                localTypes_[f.identifierToken.text] = typeText(f.type);
            }
            for (const auto& p : declaration.properties) {
                memberNames.insert(p.identifierToken.text);
                localTypes_[p.identifierToken.text] = typeText(p.type);
            }
            bool hasGenerics = false;
            ast_monomorphizer::arrayRankOverflowFlag() = false;
            ast_monomorphizer::substituteFunctionDeclaration(
                method, subs, memberNames, hasGenerics);
            if (ast_monomorphizer::arrayRankOverflowFlag()) {
                ast_monomorphizer::arrayRankOverflowFlag() = false;
                result_.diagnostics.report(
                    "RS8511",
                    "generic member specialization '" + pending.generatedName +
                        "' produces nested array type (rank > 1), which is not supported",
                    pending.requestSpan,
                    diagnostics::DiagnosticSeverity::Error,
                    pending.sourceName);
                modules_[pending.consumerModule].invalid = true;
                return true;
            }
            declaration.methods.push_back(std::move(method));
            rewriteFunction(
                declaration.methods.back(),
                pending.declaration->moduleName,
                pending.declaration->sourceName,
                queue);
            return true;
        };

        for (auto& declaration : ownerUnit->syntaxTree->classes) {
            if (rewriteSpecializedMemberOn(declaration)) return;
        }
        for (auto& declaration : ownerUnit->syntaxTree->structs) {
            if (rewriteSpecializedMemberOn(declaration)) return;
        }
    }

    void clearGenericType(syntax::TypeSyntax& type, std::string name) {
        type.name.text = std::move(name);
        type.lessToken.reset();
        type.typeArguments.clear();
        type.typeArgumentCommaTokens.clear();
        type.greaterToken.reset();
    }

    bool rewriteType(
        syntax::TypeSyntax& type,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue,
        std::size_t syntacticDepth = 1u) {
        if (!type.isGeneric()) return true;
        if (currentGenericDepth_ + syntacticDepth > 32u) {
            result_.diagnostics.report(
                "RS8512",
                "generic specialization depth limit (32) exceeded while instantiating '" +
                    type.name.text + "'",
                type.span(),
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            modules_[moduleName].invalid = true;
            clearGenericType(type, type.name.text);
            return false;
        }
        for (auto& argument : type.typeArguments) {
            if (!rewriteType(argument, moduleName, sourceName, queue, syntacticDepth + 1u)) {
                clearGenericType(type, type.name.text);
                return false;
            }
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
        return true;
    }

    void rewritePattern(
        syntax::PatternSyntax& pattern,
        const std::string& inputType,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        switch (pattern.kind()) {
        case syntax::SyntaxKind::ConstantPattern: {
            auto& cp = static_cast<syntax::ConstantPatternSyntax&>(pattern);
            if (cp.expression) rewriteExpression(*cp.expression, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::RelationalPattern: {
            auto& rp = static_cast<syntax::RelationalPatternSyntax&>(pattern);
            if (rp.operand) rewriteExpression(*rp.operand, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::VarPattern: {
            auto& vp = static_cast<syntax::VarPatternSyntax&>(pattern);
            if (!inputType.empty() && !vp.designationToken.text.empty() &&
                vp.designationToken.text != "_") {
                localTypes_[vp.designationToken.text] = inputType;
            }
            return;
        }
        case syntax::SyntaxKind::TypePattern: {
            auto& tp = static_cast<syntax::TypePatternSyntax&>(pattern);
            rewriteType(tp.type, moduleName, sourceName, queue);
            if (tp.designationToken && !tp.designationToken->text.empty() &&
                tp.designationToken->text != "_") {
                localTypes_[tp.designationToken->text] = typeText(tp.type);
            }
            return;
        }
        case syntax::SyntaxKind::PropertyPattern: {
            auto& pp = static_cast<syntax::PropertyPatternSyntax&>(pattern);
            std::string effectiveType = inputType;
            if (pp.type) {
                rewriteType(*pp.type, moduleName, sourceName, queue);
                effectiveType = typeText(*pp.type);
            }
            if (pp.designationToken && !pp.designationToken->text.empty() &&
                pp.designationToken->text != "_" && !effectiveType.empty()) {
                localTypes_[pp.designationToken->text] = effectiveType;
            }
            for (auto& sub : pp.properties) {
                const auto memberType = findFieldOrPropertyType(
                    effectiveType, sub.memberToken.text);
                if (sub.pattern) {
                    rewritePattern(*sub.pattern, memberType, moduleName, sourceName, queue);
                }
            }
            return;
        }
        case syntax::SyntaxKind::UnaryPattern: {
            auto& un = static_cast<syntax::UnaryPatternSyntax&>(pattern);
            if (un.pattern) rewritePattern(*un.pattern, inputType, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::BinaryPattern: {
            auto& bin = static_cast<syntax::BinaryPatternSyntax&>(pattern);
            if (bin.left) rewritePattern(*bin.left, inputType, moduleName, sourceName, queue);
            if (bin.right) rewritePattern(*bin.right, inputType, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::ParenthesizedPattern: {
            auto& paren = static_cast<syntax::ParenthesizedPatternSyntax&>(pattern);
            if (paren.pattern) {
                rewritePattern(*paren.pattern, inputType, moduleName, sourceName, queue);
            }
            return;
        }
        default:
            return;
        }
    }

    std::vector<const NativeGenericTemplate*> collectExtensionTemplates(
        const std::string& moduleName,
        const std::string& methodName) const {
        std::vector<const NativeGenericTemplate*> out;
        std::set<const NativeGenericTemplate*> seen;
        const auto scanModule = [&](const std::string& modName) {
            if (const auto memMod = registry_.members.find(modName);
                memMod != registry_.members.end()) {
                if (const auto found = memMod->second.find(methodName);
                    found != memMod->second.end()) {
                    for (const auto& tmpl : found->second) {
                        if (tmpl.isExtension && seen.insert(&tmpl).second) {
                            out.push_back(&tmpl);
                        }
                    }
                }
            }
            if (const auto fnMod = registry_.functions.find(modName);
                fnMod != registry_.functions.end()) {
                if (const auto found = fnMod->second.find(methodName);
                    found != fnMod->second.end() &&
                    found->second.isExtension &&
                    seen.insert(&found->second).second) {
                    out.push_back(&found->second);
                }
            }
        };
        scanModule(moduleName);
        if (const auto modIt = modules_.find(moduleName); modIt != modules_.end()) {
            for (const auto& imp : modIt->second.imports) scanModule(imp);
            for (const auto& hyg : modIt->second.hygienicModules) scanModule(hyg);
        }
        if (currentRewritingUnit_) {
            for (const auto& hyg : currentRewritingUnit_->unitHygienicModules) scanModule(hyg);
        }
        for (const auto& hyg : currentMethodHygienicModules_) scanModule(hyg);
        return out;
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
        case syntax::SyntaxKind::TypeBinaryExpression: {
            auto& value = static_cast<syntax::TypeBinaryExpressionSyntax&>(expression);
            rewriteExpression(*value.expression, moduleName, sourceName, queue);
            const auto inputType = expressionType(*value.expression);
            if (value.pattern) {
                rewritePattern(*value.pattern, inputType, moduleName, sourceName, queue);
                if (value.pattern->kind() == syntax::SyntaxKind::TypePattern) {
                    value.type = static_cast<const syntax::TypePatternSyntax&>(*value.pattern).type;
                }
            } else {
                rewriteType(value.type, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::TaggedStringExpression:
            return;
        case syntax::SyntaxKind::TypeOfExpression: {
            auto& value = static_cast<syntax::TypeOfExpressionSyntax&>(expression);
            rewriteType(value.type, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::SwitchExpression: {
            auto& value = static_cast<syntax::SwitchExpressionSyntax&>(expression);
            rewriteExpression(*value.expression, moduleName, sourceName, queue);
            const auto inputType = expressionType(*value.expression);
            for (auto& arm : value.arms) {
                const auto savedLocal = localTypes_;
                if (arm.pattern) {
                    rewritePattern(*arm.pattern, inputType, moduleName, sourceName, queue);
                    if (arm.pattern->kind() == syntax::SyntaxKind::TypePattern) {
                        arm.patternType = static_cast<const syntax::TypePatternSyntax&>(*arm.pattern).type;
                    }
                } else {
                    if (arm.patternType) rewriteType(*arm.patternType, moduleName, sourceName, queue);
                    if (arm.label) rewriteExpression(*arm.label, moduleName, sourceName, queue);
                }
                if (arm.guard) rewriteExpression(*arm.guard, moduleName, sourceName, queue);
                if (arm.value) rewriteExpression(*arm.value, moduleName, sourceName, queue);
                localTypes_ = savedLocal;
            }
            return;
        }
        case syntax::SyntaxKind::ConditionalExpression: {
            auto& value = static_cast<syntax::ConditionalExpressionSyntax&>(expression);
            rewriteExpression(*value.condition, moduleName, sourceName, queue);
            rewriteExpression(*value.whenTrue, moduleName, sourceName, queue);
            rewriteExpression(*value.whenFalse, moduleName, sourceName, queue);
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
        case syntax::SyntaxKind::CastExpression: {
            auto& value = static_cast<syntax::CastExpressionSyntax&>(expression);
            rewriteType(value.type, moduleName, sourceName, queue);
            rewriteExpression(*value.expression, moduleName, sourceName, queue);
            return;
        }
        case syntax::SyntaxKind::NameExpression: {
            auto& nameExpr = static_cast<syntax::NameExpressionSyntax&>(expression);
            if (nameExpr.lessToken) {
                for (auto& argument : nameExpr.typeArguments) {
                    rewriteType(argument, moduleName, sourceName, queue);
                }
                std::vector<std::string> arguments;
                for (const auto& argument : nameExpr.typeArguments) {
                    arguments.push_back(typeText(argument));
                }
                nameExpr.identifierToken.text = request(
                    moduleName,
                    nameExpr.identifierToken.text,
                    std::move(arguments),
                    false,
                    nameExpr.span(),
                    sourceName,
                    queue);
                nameExpr.lessToken.reset();
                nameExpr.typeArguments.clear();
                nameExpr.typeArgumentCommaTokens.clear();
                nameExpr.greaterToken.reset();
            }
            return;
        }
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
                const auto inferred = inferArguments(*declaration, call.arguments, moduleName);
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
                if (argument && argument->kind() != syntax::SyntaxKind::LambdaExpression) {
                    rewriteExpression(*argument, moduleName, sourceName, queue);
                }
            }
            auto receiverType = expressionType(*call.receiver);
            bool staticTypeReceiver = false;
            if (receiverType.empty() && call.receiver &&
                call.receiver->kind() == syntax::SyntaxKind::NameExpression) {
                const auto& rName = static_cast<const syntax::NameExpressionSyntax&>(
                    *call.receiver).identifierToken.text;
                if (localTypes_.find(rName) == localTypes_.end()) {
                    receiverType = rName;
                    staticTypeReceiver = true;
                }
            }

            const NativeGenericTemplate* declaration = nullptr;
            if (!receiverType.empty()) {
                declaration = resolveMember(
                    moduleName,
                    call.nameToken.text,
                    receiverType,
                    call.span(),
                    sourceName);
                if (declaration && declaration->isExtension && !staticTypeReceiver) {
                    declaration = nullptr;
                }
            }

            std::vector<std::string> explicitTypeArgs;
            if (call.lessToken) {
                for (auto& argument : call.typeArguments) {
                    rewriteType(argument, moduleName, sourceName, queue);
                    explicitTypeArgs.push_back(typeText(argument));
                }
            }

            if (declaration) {
                if (call.lessToken) {
                    call.nameToken.text = requestMember(
                        moduleName, *declaration, explicitTypeArgs,
                        call.span(), sourceName);
                } else if (const auto inferred = inferArguments(
                               *declaration, call.arguments, moduleName)) {
                    call.nameToken.text = requestMember(
                        moduleName, *declaration, *inferred,
                        call.span(), sourceName);
                }
            } else if (!staticTypeReceiver) {
                auto extTemplates = collectExtensionTemplates(moduleName, call.nameToken.text);
                std::vector<const syntax::ExpressionSyntax*> combinedArgs;
                combinedArgs.reserve(call.arguments.size() + 1);
                combinedArgs.push_back(call.receiver.get());
                for (const auto& a : call.arguments) combinedArgs.push_back(a.get());

                const NativeGenericTemplate* matchedExt = nullptr;
                std::vector<std::string> matchedTypeArgs;
                for (const auto* ext : extTemplates) {
                    if (ext->parameterTypeSyntaxes.size() != combinedArgs.size()) continue;
                    if (call.lessToken) {
                        if (ext->parameters.size() != explicitTypeArgs.size()) continue;
                        if (!receiverType.empty() && !ext->parameterTypeSyntaxes.empty()) {
                            std::set<std::string> tpSet(
                                ext->parameters.begin(), ext->parameters.end());
                            std::map<std::string, std::string> prefilled;
                            for (std::size_t ti = 0; ti < ext->parameters.size(); ++ti) {
                                prefilled[ext->parameters[ti]] = explicitTypeArgs[ti];
                            }
                            if (!matchFormalTypeAgainstActual(
                                    ext->parameterTypeSyntaxes[0],
                                    receiverType,
                                    tpSet,
                                    prefilled)) {
                                continue;
                            }
                        }
                        matchedExt = ext;
                        matchedTypeArgs = explicitTypeArgs;
                        break;
                    }
                    if (auto inferred = inferCallTypeArguments(*ext, combinedArgs, moduleName)) {
                        matchedExt = ext;
                        matchedTypeArgs = std::move(*inferred);
                        break;
                    }
                }

                if (matchedExt) {
                    if (!matchedExt->ownerName.empty()) {
                        call.nameToken.text = requestMember(
                            moduleName, *matchedExt, matchedTypeArgs,
                            call.span(), sourceName);
                    } else {
                        const std::string qualifiedFn = matchedExt->moduleName.empty()
                            ? matchedExt->name
                            : (matchedExt->moduleName + "::" + matchedExt->name);
                        call.nameToken.text = stripCanonicalModulePrefix(request(
                            moduleName,
                            qualifiedFn,
                            matchedTypeArgs,
                            true,
                            call.span(),
                            sourceName,
                            queue));
                    }
                } else if (receiverType.empty()) {
                    if (const auto* fallback = resolveMember(
                            moduleName, call.nameToken.text, {}, call.span(), sourceName)) {
                        if (call.lessToken) {
                            call.nameToken.text = requestMember(
                                moduleName, *fallback, explicitTypeArgs,
                                call.span(), sourceName);
                        } else if (const auto inferred = inferArguments(
                                       *fallback, call.arguments, moduleName)) {
                            call.nameToken.text = requestMember(
                                moduleName, *fallback, *inferred,
                                call.span(), sourceName);
                        }
                    }
                }
            }

            if (call.lessToken) {
                call.lessToken.reset();
                call.typeArguments.clear();
                call.typeArgumentCommaTokens.clear();
                call.greaterToken.reset();
            }
            for (auto& argument : call.arguments) {
                if (argument && argument->kind() == syntax::SyntaxKind::LambdaExpression) {
                    rewriteExpression(*argument, moduleName, sourceName, queue);
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
            const auto inputType = expressionType(*value.expression);
            for (auto& section : value.sections) {
                const auto savedLocal = localTypes_;
                if (section.pattern) {
                    rewritePattern(*section.pattern, inputType, moduleName, sourceName, queue);
                    if (section.pattern->kind() == syntax::SyntaxKind::TypePattern) {
                        section.patternType = static_cast<const syntax::TypePatternSyntax&>(
                            *section.pattern).type;
                    }
                } else {
                    if (section.patternType) rewriteType(*section.patternType, moduleName, sourceName, queue);
                    if (section.label) rewriteExpression(*section.label, moduleName, sourceName, queue);
                }
                if (section.guard) rewriteExpression(*section.guard, moduleName, sourceName, queue);
                for (auto& child : section.statements) rewriteStatement(*child, moduleName, sourceName, queue);
                localTypes_ = savedLocal;
            }
            return;
        }
        case syntax::SyntaxKind::ThrowStatement: {
            auto& value = static_cast<syntax::ThrowStatementSyntax&>(statement);
            if (value.expression) {
                rewriteExpression(*value.expression, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::TryStatement: {
            auto& value = static_cast<syntax::TryStatementSyntax&>(statement);
            rewriteStatement(value.body, moduleName, sourceName, queue);
            for (auto& c : value.catches) {
                if (c.type) rewriteType(*c.type, moduleName, sourceName, queue);
                if (c.identifierToken && c.type) {
                    localTypes_[c.identifierToken->text] = typeText(*c.type);
                }
                rewriteStatement(c.body, moduleName, sourceName, queue);
            }
            if (value.finallyBody) {
                rewriteStatement(*value.finallyBody, moduleName, sourceName, queue);
            }
            return;
        }
        case syntax::SyntaxKind::YieldWaitStatement: {
            auto& value = static_cast<syntax::YieldWaitStatementSyntax&>(statement);
            rewriteExpression(*value.delay, moduleName, sourceName, queue);
            if (value.secondArgument) {
                rewriteExpression(*value.secondArgument, moduleName, sourceName, queue);
            }
            return;
        }
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
            if (value.initializer) rewriteExpression(*value.initializer, moduleName, sourceName, queue);
            auto declaredText = typeText(value.type);
            if (declaredText == "var" && value.initializer) {
                const auto inferred = expressionType(*value.initializer);
                if (!inferred.empty()) declaredText = inferred;
            }
            localTypes_[value.identifierToken.text] = std::move(declaredText);
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
        if (parameter.defaultValue) {
            rewriteExpression(*parameter.defaultValue, moduleName, sourceName, queue);
        }
    }

    void rewriteFunction(
        syntax::FunctionDeclarationSyntax& function,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        if (!function.typeParameters.empty()) return;
        const auto previousLocalTypes = localTypes_;
        localTypes_.clear();
        if (!currentOwnerTypeName_.empty() && !function.staticKeyword) {
            localTypes_["this"] = currentOwnerTypeName_;
        }
        rewriteType(function.returnType, moduleName, sourceName, queue);
        for (auto& parameter : function.parameters) {
            rewriteParameter(parameter, moduleName, sourceName, queue);
            localTypes_[parameter.identifierToken.text] = typeText(parameter.type);
        }
        if (function.hasBody()) {
            rewriteStatement(function.body, moduleName, sourceName, queue);
        }
        localTypes_ = previousLocalTypes;
    }

    template <typename TypeDeclaration>
    void rewriteTypeDeclaration(
        TypeDeclaration& declaration,
        const std::string& moduleName,
        const std::string& sourceName,
        std::vector<ParsedUnit*>& queue) {
        if (!declaration.typeParameters.empty()) return;
        const auto savedOwnerName = currentOwnerTypeName_;
        currentOwnerTypeName_ = declaration.identifierToken.text;
        for (auto& interfaceType : declaration.interfaces) rewriteType(interfaceType, moduleName, sourceName, queue);
        for (auto& field : declaration.fields) {
            rewriteType(field.type, moduleName, sourceName, queue);
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
            for (auto& parameter : constructor.parameters) rewriteParameter(parameter, moduleName, sourceName, queue);
            for (auto& baseArg : constructor.baseArguments) {
                if (baseArg) rewriteExpression(*baseArg, moduleName, sourceName, queue);
            }
            rewriteStatement(constructor.body, moduleName, sourceName, queue);
        }
        for (auto& property : declaration.properties) {
            rewriteType(property.type, moduleName, sourceName, queue);
            if (property.getter && property.getter->body) rewriteStatement(*property.getter->body, moduleName, sourceName, queue);
            if (property.setter && property.setter->body) rewriteStatement(*property.setter->body, moduleName, sourceName, queue);
            if (property.initializer) rewriteExpression(*property.initializer, moduleName, sourceName, queue);
        }
        currentOwnerTypeName_ = savedOwnerName;
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
        const auto* prevUnit = currentRewritingUnit_;
        const auto prevDepth = currentGenericDepth_;
        currentRewritingUnit_ = &unit;
        currentGenericDepth_ = unit.genericDepth;
        detachSyntaxTree(unit);
        const auto& moduleName = unit.moduleName;
        const auto sourceName = unit.source->name();
        for (auto& declaration : unit.syntaxTree->classes) rewriteTypeDeclaration(declaration, moduleName, sourceName, queue);
        for (auto& declaration : unit.syntaxTree->structs) rewriteTypeDeclaration(declaration, moduleName, sourceName, queue);
        for (auto& declaration : unit.syntaxTree->interfaces) {
            if (!declaration.typeParameters.empty()) continue;
            const auto savedOwner = currentOwnerTypeName_;
            currentOwnerTypeName_ = declaration.identifierToken.text;
            for (auto& method : declaration.methods) {
                const auto savedLocal = localTypes_;
                localTypes_.clear();
                localTypes_["this"] = declaration.identifierToken.text;
                rewriteType(method.returnType, moduleName, sourceName, queue);
                for (auto& parameter : method.parameters) {
                    rewriteParameter(parameter, moduleName, sourceName, queue);
                    localTypes_[parameter.identifierToken.text] = typeText(parameter.type);
                }
                if (method.hasBody()) {
                    rewriteStatement(*method.body, moduleName, sourceName, queue);
                }
                localTypes_ = savedLocal;
            }
            currentOwnerTypeName_ = savedOwner;
        }
        for (auto& declaration : unit.syntaxTree->delegates) {
            if (!declaration.typeParameters.empty()) continue;
            rewriteType(declaration.returnType, moduleName, sourceName, queue);
            for (auto& parameter : declaration.parameters) rewriteParameter(parameter, moduleName, sourceName, queue);
        }
        for (auto& function : unit.syntaxTree->functions) rewriteFunction(function, moduleName, sourceName, queue);
        currentRewritingUnit_ = prevUnit;
        currentGenericDepth_ = prevDepth;
    }

    std::vector<std::unique_ptr<ParsedUnit>>& units_;
    std::map<std::string, ModuleWork>& modules_;
    BuildResult& result_;
    NativeGenericRegistry registry_;
    std::set<std::string> specializations_;
    std::set<ParsedUnit*> processed_;
    std::map<std::string, std::string> localTypes_;
    std::deque<PendingNativeGenericMember> pendingMembers_;
    const ParsedUnit* currentRewritingUnit_ = nullptr;
    std::uint32_t currentGenericDepth_ = 0;
    std::set<std::string> currentMethodHygienicModules_;
    std::string currentOwnerTypeName_;
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> instantiatedTypes_;
    std::map<std::string, std::string> specializedReturnTypes_;
};


} // namespace

void specializeNativeGenerics(
    std::vector<std::unique_ptr<ParsedUnit>>& units,
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    NativeGenericSpecializer specializer(units, modules, result);
    specializer.run();
}

} // namespace realscript::compiler::detail
