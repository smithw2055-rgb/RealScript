#pragma once

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/LanguageMetadata.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/semantic/Semantic.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realscript::reflection {

struct AttributeReflection {
    std::string name;
    std::string target;
    std::string sourceName;
    std::size_t offset = 0;
    std::vector<compiler::LanguageAttributeArgument> arguments;
    std::map<std::string, std::string> argumentMap;

    [[nodiscard]] bool hasArgument(std::string_view key = "arg0") const noexcept;
    [[nodiscard]] std::optional<std::string> getRaw(std::string_view key = "arg0") const;
    [[nodiscard]] std::optional<std::string> getString(std::string_view key = "arg0") const;
    [[nodiscard]] std::optional<std::int64_t> getInt(std::string_view key = "arg0") const;
    [[nodiscard]] std::optional<double> getDouble(std::string_view key = "arg0") const;
    [[nodiscard]] std::optional<bool> getBool(std::string_view key = "arg0") const;

    [[nodiscard]] std::optional<std::string> getStringAt(std::size_t positionalIndex) const;
    [[nodiscard]] std::optional<std::int64_t> getIntAt(std::size_t positionalIndex) const;
    [[nodiscard]] std::optional<double> getDoubleAt(std::size_t positionalIndex) const;
    [[nodiscard]] std::optional<bool> getBoolAt(std::size_t positionalIndex) const;
};

struct FieldReflection {
    semantic::SymbolId id = 0;
    semantic::SymbolId declaringTypeId = 0;
    std::string declaringTypeName;
    std::string name;
    semantic::Accessibility accessibility = semantic::Accessibility::Public;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId fieldTypeId = 0;
    std::string typeName;
    std::size_t offsetIndex = 0;
    std::size_t index = 0;
    bool isStatic = false;
    bool isConst = false;
    bool isReadonly = false;
    bool isSynthetic = false;
    bool isBackingField = false;
    std::string backedPropertyName;
    bool hasConstantValue = false;
    syntax::TokenValue constantValue;
    std::vector<AttributeReflection> attributes;

    [[nodiscard]] bool hasAttribute(std::string_view attrName) const noexcept;
    [[nodiscard]] const AttributeReflection* findAttribute(std::string_view attrName) const noexcept;
};

struct PropertyReflection {
    semantic::SymbolId id = 0;
    semantic::SymbolId declaringTypeId = 0;
    std::string declaringTypeName;
    std::string name;
    semantic::Accessibility accessibility = semantic::Accessibility::Public;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId propertyTypeId = 0;
    std::string typeName;
    bool isStatic = false;
    bool hasBackingField = false;
    std::size_t backingFieldIndex = 0;
    std::string backingFieldName;
    std::optional<semantic::SymbolId> getterSymbolId;
    std::optional<semantic::SymbolId> setterSymbolId;
    std::vector<AttributeReflection> attributes;

    [[nodiscard]] bool hasAttribute(std::string_view attrName) const noexcept;
    [[nodiscard]] const AttributeReflection* findAttribute(std::string_view attrName) const noexcept;
};

struct ParameterReflection {
    std::string name;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
    std::string typeName;
    semantic::ParameterModifier modifier = semantic::ParameterModifier::None;
    std::size_t index = 0;
    bool hasDefaultValue = false;
};

struct MethodReflection {
    semantic::SymbolId symbolId = 0;
    semantic::SymbolId declaringTypeId = 0;
    semantic::SymbolId ownerTypeId = 0;
    std::string moduleName;
    std::string ownerTypeName;
    std::string name;
    std::string qualifiedName;
    semantic::Accessibility accessibility = semantic::Accessibility::Public;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    std::string returnTypeName;
    std::vector<ParameterReflection> parameters;
    bool isStatic = false;
    bool isConstructor = false;
    bool isVirtual = false;
    bool isOverride = false;
    bool isAbstract = false;
    bool isSealed = false;
    bool isInterfaceMethod = false;
    bool isPropertyAccessor = false;
    bool isSynthetic = false;
    bool isAsync = false;
    bool isDeterministic = false;
    std::optional<std::uint32_t> virtualSlot;
    std::optional<std::uint32_t> interfaceSlot;
    std::vector<AttributeReflection> attributes;

    [[nodiscard]] std::size_t arity() const noexcept { return parameters.size(); }
    [[nodiscard]] bool hasAttribute(std::string_view attrName) const noexcept;
    [[nodiscard]] const AttributeReflection* findAttribute(std::string_view attrName) const noexcept;
};

struct TypeReflection {
    semantic::SymbolId typeId = 0;
    std::string moduleName;
    std::string name;
    std::string canonicalName;
    semantic::TypeKind kind = semantic::TypeKind::Class;
    semantic::Accessibility accessibility = semantic::Accessibility::Public;
    bool isInterface = false;
    bool isAbstract = false;
    bool isSealed = false;
    bool isStatic = false;
    bool isDelegate = false;
    bool isSynthetic = false;
    semantic::SymbolId baseTypeId = 0;
    std::string baseTypeName;
    std::vector<semantic::SymbolId> implementedInterfaceIds;
    std::vector<std::string> implementedInterfaces;
    std::vector<FieldReflection> fields;
    std::vector<PropertyReflection> properties;
    std::vector<MethodReflection> constructors;
    std::vector<MethodReflection> methods;
    std::vector<semantic::EnumMemberSymbol> enumMembers;
    std::vector<semantic::SymbolId> virtualDispatchTable;
    std::vector<semantic::InterfaceDispatchMap> interfaceDispatchMaps;
    std::optional<semantic::SymbolId> staticConstructorSymbolId;
    std::vector<AttributeReflection> attributes;

    [[nodiscard]] const FieldReflection* findField(std::string_view fieldName) const noexcept;
    [[nodiscard]] const PropertyReflection* findProperty(std::string_view propertyName) const noexcept;
    [[nodiscard]] const MethodReflection* findMethod(
        std::string_view methodName,
        std::optional<std::size_t> arity = std::nullopt) const noexcept;
    [[nodiscard]] std::vector<const MethodReflection*> findMethods(
        std::string_view methodName) const;
    [[nodiscard]] bool hasAttribute(std::string_view attrName) const noexcept;
    [[nodiscard]] const AttributeReflection* findAttribute(std::string_view attrName) const noexcept;
};

struct AnnotatedTypeMatch {
    const TypeReflection* type = nullptr;
    const AttributeReflection* attribute = nullptr;
};

struct AnnotatedFieldMatch {
    const TypeReflection* ownerType = nullptr;
    const FieldReflection* field = nullptr;
    const AttributeReflection* attribute = nullptr;
};

struct AnnotatedMethodMatch {
    const TypeReflection* ownerType = nullptr;
    const MethodReflection* method = nullptr;
    const AttributeReflection* attribute = nullptr;
};

class ReflectionRegistry {
public:
    ReflectionRegistry() = default;
    explicit ReflectionRegistry(
        std::shared_ptr<const runtime::ProgramImage> program,
        std::shared_ptr<runtime::ManagedHeap> defaultHeap = nullptr);
    explicit ReflectionRegistry(const runtime::EngineRuntime& runtime);

    [[nodiscard]] static std::shared_ptr<ReflectionRegistry> fromProgram(
        std::shared_ptr<const runtime::ProgramImage> program,
        std::shared_ptr<runtime::ManagedHeap> defaultHeap = nullptr);

    void bindHeap(std::shared_ptr<runtime::ManagedHeap> heap) const;
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> resolveBoundHeap(
        std::uint32_t heapId = 0) const;

    [[nodiscard]] std::shared_ptr<const runtime::ProgramImage> program() const noexcept {
        return program_;
    }
    [[nodiscard]] const std::vector<TypeReflection>& types() const noexcept {
        return types_;
    }
    [[nodiscard]] const std::vector<MethodReflection>& moduleFunctions() const noexcept {
        return moduleFunctions_;
    }
    [[nodiscard]] std::uint64_t metadataDigest() const noexcept {
        return metadataDigest_;
    }

    // Deterministic type & symbol lookup
    [[nodiscard]] const TypeReflection* findType(semantic::SymbolId typeId) const noexcept;
    [[nodiscard]] const TypeReflection* findType(std::string_view nameOrCanonical) const noexcept;
    [[nodiscard]] std::vector<const TypeReflection*> findTypesByShortName(
        std::string_view shortName) const;
    [[nodiscard]] const MethodReflection* findModuleFunction(
        std::string_view qualifiedOrModuleName,
        std::string_view funcName = {},
        std::optional<std::size_t> arity = std::nullopt) const noexcept;

    [[nodiscard]] bool isTypeAssignable(
        semantic::SymbolId actualTypeId,
        semantic::SymbolId expectedTypeId) const noexcept;

    // Program-wide attribute queries (deterministic sorted order)
    [[nodiscard]] std::vector<AnnotatedTypeMatch> findTypesWithAttribute(
        std::string_view attributeName) const;
    [[nodiscard]] std::vector<AnnotatedFieldMatch> findFieldsWithAttribute(
        std::string_view attributeName) const;
    [[nodiscard]] std::vector<AnnotatedMethodMatch> findMethodsWithAttribute(
        std::string_view attributeName) const;

    [[nodiscard]] const AttributeReflection* findAttributeOnTarget(
        std::string_view targetSpec,
        std::string_view attributeName,
        bool allowNonPublic = false) const noexcept;
    [[nodiscard]] bool hasAttributeOnTarget(
        std::string_view targetSpec,
        std::string_view attributeName,
        bool allowNonPublic = false) const noexcept;

    // Type-safe dynamic instantiation
    [[nodiscard]] std::optional<runtime::ObjectRef> instantiateObject(
        runtime::EngineRuntime& runtime,
        std::string_view typeName,
        const std::vector<runtime::Value>& constructorArgs,
        runtime::RuntimeError& error,
        bool runConstructor = true,
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    [[nodiscard]] std::optional<runtime::ObjectRef> instantiateObject(
        runtime::EngineRuntime& runtime,
        semantic::SymbolId typeId,
        const std::vector<runtime::Value>& constructorArgs,
        runtime::RuntimeError& error,
        bool runConstructor = true,
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    [[nodiscard]] std::optional<runtime::ObjectRef> instantiateUninitializedObject(
        runtime::ManagedHeap& heap,
        std::string_view typeName,
        runtime::RuntimeError& error) const;

    [[nodiscard]] std::optional<runtime::ObjectRef> instantiateUninitializedObject(
        runtime::ManagedHeap& heap,
        semantic::SymbolId typeId,
        runtime::RuntimeError& error) const;

    // Type-safe field access: ObjectRef instance fields, StructValue fields, and static fields
    [[nodiscard]] std::optional<runtime::Value> getField(
        const runtime::ManagedHeap& heap,
        runtime::ObjectRef object,
        std::string_view fieldName,
        runtime::RuntimeError& error,
        bool allowNonPublic = false) const;

    bool setField(
        runtime::ManagedHeap& heap,
        runtime::ObjectRef object,
        std::string_view fieldName,
        runtime::Value value,
        runtime::RuntimeError& error,
        bool allowNonPublic = false) const;

    [[nodiscard]] std::optional<runtime::Value> getField(
        const runtime::EngineRuntime& runtime,
        runtime::ObjectRef object,
        std::string_view fieldName,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    bool setField(
        runtime::EngineRuntime& runtime,
        runtime::ObjectRef object,
        std::string_view fieldName,
        runtime::Value value,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    [[nodiscard]] std::optional<runtime::Value> getField(
        const runtime::StructValue& structure,
        std::string_view fieldName,
        runtime::RuntimeError& error,
        bool allowNonPublic = false) const;

    bool setField(
        runtime::StructValue& structure,
        std::string_view fieldName,
        runtime::Value value,
        runtime::RuntimeError& error,
        runtime::ManagedHeap* heap = nullptr,
        bool allowNonPublic = false) const;

    [[nodiscard]] std::optional<runtime::Value> getStructField(
        const runtime::Value& structValue,
        std::string_view fieldName,
        runtime::RuntimeError& error,
        bool allowNonPublic = false) const;

    bool setStructField(
        runtime::Value& structValue,
        std::string_view fieldName,
        runtime::Value value,
        runtime::RuntimeError& error,
        runtime::ManagedHeap* heap = nullptr,
        bool allowNonPublic = false) const;

    [[nodiscard]] std::optional<runtime::Value> getStaticField(
        runtime::ManagedHeap& heap,
        std::string_view typeName,
        std::string_view fieldName,
        runtime::RuntimeError& error,
        const runtime::EngineRuntime* runtime = nullptr,
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    bool setStaticField(
        runtime::ManagedHeap& heap,
        std::string_view typeName,
        std::string_view fieldName,
        runtime::Value value,
        runtime::RuntimeError& error,
        const runtime::EngineRuntime* runtime = nullptr,
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    // Dynamic & polymorphic method invocation (static, instance, virtual vtable & interface slot dispatch)
    [[nodiscard]] runtime::ExecutionResult invokeMethod(
        runtime::EngineRuntime& runtime,
        runtime::ObjectRef receiver,
        std::string_view methodName,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    [[nodiscard]] runtime::ExecutionResult invokeMethod(
        runtime::EngineRuntime& runtime,
        const MethodReflection& declaredMethod,
        runtime::ObjectRef receiver,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    [[nodiscard]] runtime::ExecutionResult invokeStaticMethod(
        runtime::EngineRuntime& runtime,
        std::string_view typeOrModuleName,
        std::string_view methodName,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

    [[nodiscard]] runtime::ExecutionResult invokeMethod(
        runtime::EngineRuntime& runtime,
        std::string_view typeOrModuleName,
        std::string_view methodName,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {},
        bool allowNonPublic = false) const;

private:
    void buildFromProgram();
    void computeMetadataDigest() noexcept;

    [[nodiscard]] const TypeReflection* resolveDeclaringTypeForField(
        const TypeReflection& ownerType,
        const FieldReflection& field) const noexcept;
    [[nodiscard]] bool isStaticInitChainInitializedOnHeap(
        const runtime::ManagedHeap& heap,
        const TypeReflection& type) const noexcept;
    [[nodiscard]] bool ensureStaticInitChain(
        const TypeReflection& type,
        runtime::ManagedHeap& heap,
        const runtime::EngineRuntime* runtime,
        const runtime::ExecutionOptions& options,
        runtime::RuntimeError& error) const;

    [[nodiscard]] runtime::Value makeDefaultFieldValue(
        semantic::PrimitiveType type,
        semantic::SymbolId typeId,
        std::uint32_t depth = 0) const;

    [[nodiscard]] std::optional<runtime::Value> evaluateConstantFieldValue(
        const FieldReflection& field,
        runtime::ManagedHeap* heap,
        runtime::RuntimeError& error) const;

    [[nodiscard]] bool structContainsManagedReferences(
        semantic::SymbolId structTypeId,
        std::uint32_t depth = 0) const noexcept;

    [[nodiscard]] bool validateAndCoerceValue(
        const runtime::Value& input,
        semantic::PrimitiveType expectedType,
        semantic::SymbolId expectedTypeId,
        runtime::ManagedHeap* targetHeap,
        runtime::Value& outCoerced,
        runtime::RuntimeError& error) const;

    std::shared_ptr<const runtime::ProgramImage> program_;
    std::vector<TypeReflection> types_;
    std::vector<MethodReflection> moduleFunctions_;
    std::unordered_map<semantic::SymbolId, std::size_t> typeIndexById_;
    std::unordered_map<std::string, std::size_t> typeIndexByCanonical_;
    std::unordered_map<std::string, std::vector<std::size_t>> typeIndicesByShortName_;
    std::unordered_map<std::string, std::vector<AttributeReflection>> attributesByTarget_;
    std::uint64_t metadataDigest_ = 0;

    mutable std::mutex heapMutex_;
    mutable std::weak_ptr<runtime::ManagedHeap> defaultHeap_;
    mutable std::unordered_map<std::uint32_t, std::weak_ptr<runtime::ManagedHeap>> heapsById_;
};

// Export standard .d.rscript declaration stub for `module RealScript.Reflect;`
[[nodiscard]] compiler::SourceFile exportReflectDeclarationStub();

// Register capability-gated deterministic `RealScript.Reflect` host bindings
void registerReflectBindings(
    runtime::BindingRegistry& registry,
    std::shared_ptr<const ReflectionRegistry> reflection,
    std::string requiredCapability = "reflection",
    std::shared_ptr<runtime::ManagedHeap> heap = nullptr);

void registerReflectBindings(
    runtime::BindingRegistry& registry,
    std::shared_ptr<const ReflectionRegistry> reflection,
    std::shared_ptr<runtime::ManagedHeap> heap,
    std::string requiredCapability);

} // namespace realscript::reflection
