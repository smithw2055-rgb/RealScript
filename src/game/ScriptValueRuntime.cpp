#include <algorithm>
#include "realscript/game/GameScripting.h"

#include <utility>

namespace realscript::game {
namespace {

semantic::SymbolId exactTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) {
    return semantic::isExactType(type) && !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

runtime::ExecutionResult failedResult(
    runtime::ErrorCode code,
    std::string message) {
    runtime::ExecutionResult result;
    result.error.code = code;
    result.error.message = std::move(message);
    return result;
}

bool isNullFor(semantic::PrimitiveType type, const runtime::Value& value) {
    return (type == semantic::PrimitiveType::String &&
            std::holds_alternative<runtime::NullString>(value)) ||
        (type == semantic::PrimitiveType::Object &&
            std::holds_alternative<runtime::NullObject>(value)) ||
        (type == semantic::PrimitiveType::Array &&
            std::holds_alternative<runtime::NullArray>(value));
}

} // namespace

const semantic::TypeSymbol* ScriptRuntime::findTypeById(
    semantic::SymbolId typeId) const {
    if (!program_ || typeId == 0) return nullptr;
    for (const auto& module : program_->modules()) {
        for (const auto& type : module.types) {
            if (type.id == typeId) return &type;
        }
    }
    return nullptr;
}

runtime::Value ScriptRuntime::defaultValue(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::unordered_set<semantic::SymbolId>& visiting) const {
    switch (type) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return runtime::ByteValue{};
    case semantic::PrimitiveType::SByte: return runtime::SByteValue{};
    case semantic::PrimitiveType::Short: return runtime::ShortValue{};
    case semantic::PrimitiveType::UShort: return runtime::UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return runtime::UIntValue{};
    case semantic::PrimitiveType::Long: return runtime::LongValue{};
    case semantic::PrimitiveType::ULong: return runtime::ULongValue{};
    case semantic::PrimitiveType::Float: return runtime::FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return runtime::CharValue{};
    case semantic::PrimitiveType::String: return runtime::NullString{};
    case semantic::PrimitiveType::Object: return runtime::NullObject{};
    case semantic::PrimitiveType::Array: return runtime::NullArray{};
    case semantic::PrimitiveType::Handle: {
        runtime::NativeHandle handle;
        handle.typeId = typeId;
        return handle;
    }
    case semantic::PrimitiveType::Enum:
        return runtime::EnumValue{typeId, 0};
    case semantic::PrimitiveType::Struct: {
        if (auto inlineVal = runtime::makeDefaultInlineStructValue(typeId)) {
            return *inlineVal;
        }
        if (typeId == 0 || !visiting.insert(typeId).second) {
            return runtime::StructValue{};
        }
        const auto* descriptor = findTypeById(typeId);
        if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
            visiting.erase(typeId);
            return runtime::StructValue{};
        }
        std::vector<runtime::Value> fields;
        fields.reserve(descriptor->fields.size());
        for (const auto& field : descriptor->fields) {
            fields.push_back(defaultValue(
                field.type,
                exactTypeId(field.type, field.typeName),
                visiting));
        }
        visiting.erase(typeId);
        return runtime::makeStructValueFromFields(typeId, std::move(fields));
    }
    case semantic::PrimitiveType::Null:
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return runtime::Value{};
}

bool ScriptRuntime::containsManagedReferences(
    semantic::SymbolId typeId,
    std::unordered_set<semantic::SymbolId>& visiting) const {
    if (typeId == 0 || !visiting.insert(typeId).second) return false;
    const auto* descriptor = findTypeById(typeId);
    if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
        visiting.erase(typeId);
        return false;
    }
    for (const auto& field : descriptor->fields) {
        if (field.type == semantic::PrimitiveType::String ||
            field.type == semantic::PrimitiveType::Object ||
            field.type == semantic::PrimitiveType::Array) {
            visiting.erase(typeId);
            return true;
        }
        if (field.type == semantic::PrimitiveType::Struct &&
            containsManagedReferences(
                exactTypeId(field.type, field.typeName), visiting)) {
            visiting.erase(typeId);
            return true;
        }
    }
    visiting.erase(typeId);
    return false;
}

bool ScriptRuntime::valueMatches(
    const runtime::Value& value,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) const {
    if (isNullFor(type, value)) return true;
    if (runtime::valueType(value) != type) return false;
    switch (type) {
    case semantic::PrimitiveType::Object: {
        if (typeId == 0) return true;
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto actual = reference ? heap_->objectTypeId(*reference) : std::nullopt;
        return actual && isTypeAssignable(*actual, typeId);
    }
    case semantic::PrimitiveType::Array: {
        if (typeId == 0) return true;
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto actual = reference ? heap_->arrayTypeId(*reference) : std::nullopt;
        return actual && (*actual == 0 || *actual == typeId);
    }
    case semantic::PrimitiveType::Struct: {
        const auto actualTypeId = runtime::structValueTypeId(value);
        if (!actualTypeId || *actualTypeId == 0 || *actualTypeId != typeId) return false;
        if (const auto* structure = std::get_if<runtime::StructValue>(&value)) {
            return structure->valid();
        }
        return true;
    }
    case semantic::PrimitiveType::Enum: {
        const auto* enumeration = std::get_if<runtime::EnumValue>(&value);
        return enumeration && enumeration->typeId == typeId;
    }
    case semantic::PrimitiveType::Handle: {
        const auto* handle = std::get_if<runtime::NativeHandle>(&value);
        return handle && (typeId == 0 || handle->typeId == 0 || handle->typeId == typeId);
    }
    default:
        return true;
    }
}

runtime::ExecutionResult ScriptRuntime::invokeSymbol(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!program_) {
        return failedResult(
            runtime::ErrorCode::InvalidProgram,
            "script runtime has no linked program image");
    }
    if (!options.debugger && debugger_) {
        options.debugger = debugger_;
    }
    if (!options.debugger && nativeProgram_ && backend_ != ScriptExecutionBackend::Interpreter) {
        if (nativeProgram_->hasFunction(symbolId)) {
            return nativeProgram_->invoke(symbolId, arguments, std::move(options));
        }
    }
    return interpreter_.invoke(symbolId, arguments, std::move(options));
}

runtime::Value ScriptRuntime::makeFixed(Fixed value) {
    return runtime::FixedValue{value.raw};
}

runtime::Value ScriptRuntime::makeVec2(Vec2 value) {
    return runtime::Vec2Value{value.x.raw, value.y.raw};
}

runtime::Value ScriptRuntime::makeVec3(Vec3 value) {
    return runtime::Vec3Value{value.x.raw, value.y.raw, value.z.raw};
}

runtime::Value ScriptRuntime::makeQuat(Quat value) {
    return runtime::QuatValue{value.x.raw, value.y.raw, value.z.raw, value.w.raw};
}

std::optional<Fixed> ScriptRuntime::readFixed(
    const runtime::Value& value) noexcept {
    return detail::extractFixedStructValue(value);
}

std::optional<Vec2> ScriptRuntime::readVec2(
    const runtime::Value& value) noexcept {
    if (const auto* vec2 = std::get_if<runtime::Vec2Value>(&value)) {
        return Vec2{Fixed::fromRaw(vec2->x), Fixed::fromRaw(vec2->y)};
    }
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure ||
        structure->typeId != runtime::vec2TypeId() ||
        !structure->storage || structure->storage->fields.size() != 2) {
        return std::nullopt;
    }
    const auto x = detail::extractFixedStructValue(structure->storage->fields[0]);
    const auto y = detail::extractFixedStructValue(structure->storage->fields[1]);
    if (!x || !y) return std::nullopt;
    return Vec2{*x, *y};
}

std::optional<Vec3> ScriptRuntime::readVec3(
    const runtime::Value& value) noexcept {
    if (const auto* vec3 = std::get_if<runtime::Vec3Value>(&value)) {
        return Vec3{Fixed::fromRaw(vec3->x), Fixed::fromRaw(vec3->y), Fixed::fromRaw(vec3->z)};
    }
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure ||
        structure->typeId != runtime::vec3TypeId() ||
        !structure->storage || structure->storage->fields.size() != 3) {
        return std::nullopt;
    }
    const auto x = detail::extractFixedStructValue(structure->storage->fields[0]);
    const auto y = detail::extractFixedStructValue(structure->storage->fields[1]);
    const auto z = detail::extractFixedStructValue(structure->storage->fields[2]);
    if (!x || !y || !z) return std::nullopt;
    return Vec3{*x, *y, *z};
}

std::optional<Quat> ScriptRuntime::readQuat(
    const runtime::Value& value) noexcept {
    if (const auto* quat = std::get_if<runtime::QuatValue>(&value)) {
        return Quat{
            Fixed::fromRaw(quat->x),
            Fixed::fromRaw(quat->y),
            Fixed::fromRaw(quat->z),
            Fixed::fromRaw(quat->w)};
    }
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure ||
        structure->typeId != runtime::quatTypeId() ||
        !structure->storage || structure->storage->fields.size() != 4) {
        return std::nullopt;
    }
    const auto x = detail::extractFixedStructValue(structure->storage->fields[0]);
    const auto y = detail::extractFixedStructValue(structure->storage->fields[1]);
    const auto z = detail::extractFixedStructValue(structure->storage->fields[2]);
    const auto w = detail::extractFixedStructValue(structure->storage->fields[3]);
    if (!x || !y || !z || !w) return std::nullopt;
    return Quat{*x, *y, *z, *w};
}

std::optional<Vec2> ScriptRuntime::getMemberVec2(
    const ScriptObject& object,
    const std::string& name,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    const auto value = getMember(object, name, error, std::move(options));
    if (!value) return std::nullopt;
    const auto vec = readVec2(*value);
    if (!vec) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "member '" + name + "' is not a RealScript.Math::Vec2";
        return std::nullopt;
    }
    return vec;
}

bool ScriptRuntime::setMemberVec2(
    ScriptObject& object,
    const std::string& name,
    Vec2 value,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    if (object.valid() && heap_) {
        const auto& fields = object.type().descriptor.fields;
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (fields[index].name != name) continue;
            const auto current = heap_->fieldGet(object.reference(), index);
            if (current) {
                const auto* vecStruct = std::get_if<runtime::StructValue>(&*current);
                if (vecStruct &&
                    vecStruct->typeId == semantic::stableTypeId("RealScript.Math::Vec2") &&
                    vecStruct->storage &&
                    vecStruct->storage.use_count() == 2 &&
                    vecStruct->storage->fields.size() == 2) {
                    auto* mutableVec = const_cast<runtime::StructStorage*>(
                        vecStruct->storage.get());
                    auto* fx = std::get_if<runtime::StructValue>(&mutableVec->fields[0]);
                    auto* fy = std::get_if<runtime::StructValue>(&mutableVec->fields[1]);
                    if (fx && fx->storage && fx->storage.use_count() == 1 &&
                        fx->storage->fields.size() == 1 &&
                        fy && fy->storage && fy->storage.use_count() == 1 &&
                        fy->storage->fields.size() == 1) {
                        const_cast<runtime::StructStorage*>(fx->storage.get())->fields[0] =
                            static_cast<std::int64_t>(value.x.raw);
                        const_cast<runtime::StructStorage*>(fy->storage.get())->fields[0] =
                            static_cast<std::int64_t>(value.y.raw);
                        return true;
                    }
                }
            }
            break;
        }
    }
    return setMember(object, name, makeVec2(value), error, std::move(options));
}

bool ScriptRuntime::readArrayInt(
    runtime::ObjectRef arrayRef,
    std::vector<std::int64_t>& out,
    runtime::RuntimeError& error) const {
    if (!heap_) return false;
    return heap_->readPackedIntArray(arrayRef, out, &error);
}

bool ScriptRuntime::writeArrayInt(
    runtime::ObjectRef arrayRef,
    const std::vector<std::int64_t>& values,
    runtime::RuntimeError& error) const {
    if (!heap_) return false;
    return heap_->writePackedIntArray(arrayRef, values.data(), values.size(), &error);
}

bool ScriptRuntime::readArrayVec2(
    runtime::ObjectRef arrayRef,
    std::vector<Vec2>& out,
    runtime::RuntimeError& error) const {
    if (!heap_) return false;
    std::vector<runtime::Vec2Value> raw;
    if (!heap_->readPackedVec2Array(arrayRef, raw, &error)) return false;
    out.resize(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = Vec2{Fixed::fromRaw(raw[i].x), Fixed::fromRaw(raw[i].y)};
    }
    return true;
}

bool ScriptRuntime::writeArrayVec2(
    runtime::ObjectRef arrayRef,
    const std::vector<Vec2>& values,
    runtime::RuntimeError& error) const {
    if (!heap_) return false;
    std::vector<runtime::Vec2Value> raw(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        raw[i] = runtime::Vec2Value{values[i].x.raw, values[i].y.raw};
    }
    return heap_->writePackedVec2Array(arrayRef, raw.data(), raw.size(), &error);
}

std::optional<runtime::Value> ScriptRuntime::createStruct(
    const std::string& canonicalTypeName,
    const std::vector<runtime::Value>& fieldValues,
    runtime::RuntimeError& error) const {
    const auto type = findType(canonicalTypeName);
    if (!type || type->descriptor.kind != semantic::TypeKind::Struct) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "type '" + canonicalTypeName + "' is not a struct";
        return std::nullopt;
    }
    if (fieldValues.size() != type->descriptor.fields.size()) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "struct '" + canonicalTypeName + "' expects " +
            std::to_string(type->descriptor.fields.size()) + " fields";
        return std::nullopt;
    }
    for (std::size_t idx = 0; idx < fieldValues.size(); ++idx) {
        const auto& field = type->descriptor.fields[idx];
        const auto expectedTypeId = exactTypeId(field.type, field.typeName);
        if (!valueMatches(fieldValues[idx], field.type, expectedTypeId)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "field '" + field.name + "' type mismatch in struct '" +
                canonicalTypeName + "'";
            return std::nullopt;
        }
    }
    std::vector<runtime::Value> copy = fieldValues;
    return runtime::makeStructValueFromFields(type->descriptor.id, std::move(copy));
}

bool ScriptRuntime::setEcsComponentColumn(
    const std::string& canonicalTypeName,
    const std::vector<runtime::Value>& components,
    runtime::RuntimeError& error) const {
    if (!heap_) {
        error.code = runtime::ErrorCode::InvalidProgram;
        error.message = "script runtime has no managed heap";
        return false;
    }
    const auto type = findType(canonicalTypeName);
    if (!type) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "unknown ECS component type '" + canonicalTypeName + "'";
        return false;
    }
    const semantic::PrimitiveType elemPrim =
        type->descriptor.kind == semantic::TypeKind::Struct
            ? semantic::PrimitiveType::Struct
            : (type->descriptor.kind == semantic::TypeKind::Class
                   ? semantic::PrimitiveType::Object
                   : semantic::PrimitiveType::Enum);
    for (const auto& elem : components) {
        if (!valueMatches(elem, elemPrim, type->descriptor.id)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "ECS component column element does not match '" +
                type->canonicalName() + "'";
            return false;
        }
    }
    const std::string arrayName = semantic::arrayTypeName(elemPrim, type->canonicalName());
    const auto arrayTypeId = semantic::stableTypeId(arrayName);
    std::unordered_set<semantic::SymbolId> visiting;
    const runtime::Value defaultElem = defaultValue(elemPrim, type->descriptor.id, visiting);
    const auto arrayRef = heap_->allocateTypedArray(
        arrayTypeId,
        elemPrim,
        type->descriptor.id,
        components.size(),
        defaultElem,
        &error);
    if (!arrayRef) return false;
    for (std::size_t idx = 0; idx < components.size(); ++idx) {
        if (!heap_->arraySet(*arrayRef, idx, components[idx], &error)) {
            return false;
        }
    }
    const auto colSymbolId = semantic::stableTypeId("$ecs_column::" + type->canonicalName());
    heap_->setStaticField(colSymbolId, *arrayRef);
    return true;
}

bool ScriptRuntime::getEcsComponentColumn(
    const std::string& canonicalTypeName,
    std::vector<runtime::Value>& outComponents,
    runtime::RuntimeError& error) const {
    outComponents.clear();
    if (!heap_) {
        error.code = runtime::ErrorCode::InvalidProgram;
        error.message = "script runtime has no managed heap";
        return false;
    }
    const auto type = findType(canonicalTypeName);
    if (!type) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "unknown ECS component type '" + canonicalTypeName + "'";
        return false;
    }
    const auto colSymbolId = semantic::stableTypeId("$ecs_column::" + type->canonicalName());
    const auto val = heap_->getStaticField(colSymbolId);
    if (!val) return true;
    const auto* arrayRef = std::get_if<runtime::ObjectRef>(&*val);
    if (!arrayRef || !arrayRef->valid()) return true;
    const auto len = heap_->arrayLength(*arrayRef);
    if (!len) return false;
    outComponents.reserve(*len);
    for (std::size_t idx = 0; idx < *len; ++idx) {
        const auto elem = heap_->arrayGet(*arrayRef, idx);
        if (!elem) return false;
        outComponents.push_back(*elem);
    }
    return true;
}

bool ScriptRuntime::bindEcsRawColumn(
    const std::string& canonicalTypeName,
    void* basePtr,
    std::size_t count,
    std::size_t strideBytes,
    const std::vector<runtime::NativeFieldLayout>& fieldLayouts,
    runtime::RuntimeError& error,
    bool readOnly) const {
    if (!heap_) {
        error.code = runtime::ErrorCode::InvalidProgram;
        error.message = "script runtime has no managed heap";
        return false;
    }
    const auto type = findType(canonicalTypeName);
    if (!type || type->descriptor.kind != semantic::TypeKind::Struct) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "ECS raw column binding requires a valid script struct type '" +
            canonicalTypeName + "'";
        return false;
    }
    for (const auto& field : type->descriptor.fields) {
        if (field.staticField) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "ECS raw column struct '" + type->canonicalName() +
                "' cannot contain static or const fields";
            return false;
        }
    }
    if (fieldLayouts.size() != type->descriptor.fields.size()) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "ECS raw column field layout count does not match struct '" +
            type->canonicalName() + "'";
        return false;
    }
    runtime::NativeRawColumnView view;
    view.basePtr = basePtr;
    view.count = count;
    view.strideBytes = strideBytes;
    view.structTypeId = type->descriptor.id;
    view.fields = fieldLayouts;
    view.readOnly = readOnly;

    const auto colSymbolId = semantic::stableTypeId("$ecs_column::" + type->canonicalName());
    if (const auto existing = heap_->getStaticField(colSymbolId)) {
        if (const auto* existingRef = std::get_if<runtime::ObjectRef>(&*existing)) {
            if (heap_->updateRawColumnArray(*existingRef, view)) {
                return true;
            }
        }
    }
    const std::string arrayName = semantic::arrayTypeName(
        semantic::PrimitiveType::Struct, type->canonicalName());
    const auto arrayTypeId = semantic::stableTypeId(arrayName);
    const auto arrayRef = heap_->allocateRawColumnArray(
        arrayTypeId, type->descriptor.id, std::move(view), &error);
    if (!arrayRef) return false;
    heap_->setStaticField(colSymbolId, *arrayRef);
    return true;
}

bool ScriptRuntime::bindEcsRawColumnStandardLayout(
    const std::string& canonicalTypeName,
    void* basePtr,
    std::size_t count,
    std::size_t strideBytes,
    runtime::RuntimeError& error,
    bool readOnly,
    bool useInt32ForScriptInt) const {
    const auto type = findType(canonicalTypeName);
    if (!type || type->descriptor.kind != semantic::TypeKind::Struct) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "ECS raw column binding requires a valid script struct type '" +
            canonicalTypeName + "'";
        return false;
    }
    const auto buildLayouts = [&](bool intAs32, bool doubleAsFloat32,
                                  std::vector<runtime::NativeFieldLayout>& outLayouts,
                                  std::size_t& outTotalSize) -> bool {
        outLayouts.clear();
        outLayouts.reserve(type->descriptor.fields.size());
        std::size_t offset = 0;
        std::size_t maxAlign = 1;
        for (const auto& field : type->descriptor.fields) {
            if (field.staticField) {
                return false;
            }
            runtime::NativeFieldLayout layout;
            layout.scriptType = field.type;
            layout.scriptTypeId = exactTypeId(field.type, field.typeName);
            std::size_t fieldSize = 0;
            std::size_t fieldAlign = 1;
            switch (field.type) {
            case semantic::PrimitiveType::Int:
                layout.kind = intAs32
                    ? runtime::NativeFieldKind::Int32
                    : runtime::NativeFieldKind::Int64;
                fieldSize = intAs32 ? 4 : 8;
                fieldAlign = fieldSize;
                break;
            case semantic::PrimitiveType::Long:
                layout.kind = runtime::NativeFieldKind::Int64;
                fieldSize = 8;
                fieldAlign = 8;
                break;
            case semantic::PrimitiveType::Float:
                layout.kind = runtime::NativeFieldKind::Float32;
                fieldSize = 4;
                fieldAlign = 4;
                break;
            case semantic::PrimitiveType::Double:
                layout.kind = doubleAsFloat32
                    ? runtime::NativeFieldKind::Float32
                    : runtime::NativeFieldKind::Float64;
                fieldSize = doubleAsFloat32 ? 4 : 8;
                fieldAlign = fieldSize;
                break;
            case semantic::PrimitiveType::Bool:
                layout.kind = runtime::NativeFieldKind::Bool;
                fieldSize = 1;
                fieldAlign = 1;
                break;
            case semantic::PrimitiveType::Struct:
                if (layout.scriptTypeId == runtime::fixedTypeId()) {
                    layout.kind = runtime::NativeFieldKind::Fixed;
                    fieldSize = 4;
                    fieldAlign = 4;
                } else if (layout.scriptTypeId == runtime::fixed16TypeId()) {
                    layout.kind = runtime::NativeFieldKind::Fixed16;
                    fieldSize = 2;
                    fieldAlign = 2;
                } else if (layout.scriptTypeId == runtime::vec2TypeId()) {
                    layout.kind = runtime::NativeFieldKind::Vec2;
                    fieldSize = 8;
                    fieldAlign = 4;
                } else if (layout.scriptTypeId == runtime::vec3TypeId()) {
                    layout.kind = runtime::NativeFieldKind::Vec3;
                    fieldSize = 12;
                    fieldAlign = 4;
                } else if (layout.scriptTypeId == runtime::quatTypeId()) {
                    layout.kind = runtime::NativeFieldKind::Quat;
                    fieldSize = 16;
                    fieldAlign = 4;
                } else {
                    return false;
                }
                break;
            default:
                return false;
            }
            maxAlign = std::max(maxAlign, fieldAlign);
            const std::size_t rem = offset % fieldAlign;
            if (rem != 0) offset += (fieldAlign - rem);
            layout.byteOffset = offset;
            offset += fieldSize;
            outLayouts.push_back(layout);
        }
        const std::size_t tailRem = offset % maxAlign;
        if (tailRem != 0) offset += (maxAlign - tailRem);
        outTotalSize = offset;
        return true;
    };

    std::vector<runtime::NativeFieldLayout> layouts;
    std::size_t computedSize = 0;
    if (!buildLayouts(useInt32ForScriptInt, false, layouts, computedSize)) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "struct '" + type->canonicalName() +
            "' contains non-blittable fields that cannot be bound as a zero-copy raw column";
        return false;
    }
    if (computedSize != strideBytes) {
        std::vector<runtime::NativeFieldLayout> altLayouts;
        std::size_t altSize = 0;
        if (buildLayouts(!useInt32ForScriptInt, false, altLayouts, altSize) &&
            altSize == strideBytes) {
            layouts = std::move(altLayouts);
            computedSize = altSize;
        } else if (buildLayouts(useInt32ForScriptInt, true, altLayouts, altSize) &&
                   altSize == strideBytes) {
            layouts = std::move(altLayouts);
            computedSize = altSize;
        } else if (buildLayouts(true, true, altLayouts, altSize) &&
                   altSize == strideBytes) {
            layouts = std::move(altLayouts);
            computedSize = altSize;
        }
    }
    if (computedSize > strideBytes) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "host C++ struct stride (" + std::to_string(strideBytes) +
            " bytes) is smaller than computed layout size (" +
            std::to_string(computedSize) + " bytes) for '" + type->canonicalName() + "'";
        return false;
    }
    return bindEcsRawColumn(
        canonicalTypeName, basePtr, count, strideBytes, layouts, error, readOnly);
}

bool ScriptRuntime::setEcsEntityComponent(
    std::uint64_t entity,
    const std::string& canonicalTypeName,
    const runtime::Value& component,
    runtime::RuntimeError& error) const {
    if (!heap_) {
        error.code = runtime::ErrorCode::InvalidProgram;
        error.message = "script runtime has no managed heap";
        return false;
    }
    const auto type = findType(canonicalTypeName);
    if (!type) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "unknown ECS component type '" + canonicalTypeName + "'";
        return false;
    }
    const semantic::PrimitiveType elemPrim =
        type->descriptor.kind == semantic::TypeKind::Struct
            ? semantic::PrimitiveType::Struct
            : (type->descriptor.kind == semantic::TypeKind::Class
                   ? semantic::PrimitiveType::Object
                   : semantic::PrimitiveType::Enum);
    if (!valueMatches(component, elemPrim, type->descriptor.id)) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "ECS component value does not match '" + type->canonicalName() + "'";
        return false;
    }

    return heap_->setNativeEcsComponent(
        type->canonicalName(), static_cast<std::int64_t>(entity), component);
}

std::optional<runtime::Value> ScriptRuntime::getEcsEntityComponent(
    std::uint64_t entity,
    const std::string& canonicalTypeName,
    runtime::RuntimeError& error) const {
    if (!heap_) {
        error.code = runtime::ErrorCode::InvalidProgram;
        error.message = "script runtime has no managed heap";
        return std::nullopt;
    }
    const auto type = findType(canonicalTypeName);
    if (!type) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "unknown ECS component type '" + canonicalTypeName + "'";
        return std::nullopt;
    }
    return heap_->getNativeEcsComponent(
        type->canonicalName(), static_cast<std::int64_t>(entity));
}

bool ScriptRuntime::hasEcsEntityComponent(
    std::uint64_t entity,
    const std::string& canonicalTypeName) const {
    if (!heap_) return false;
    const auto type = findType(canonicalTypeName);
    if (!type) return false;
    return heap_->hasNativeEcsComponent(
        type->canonicalName(), static_cast<std::int64_t>(entity));
}

bool ScriptRuntime::removeEcsEntityComponent(
    std::uint64_t entity,
    const std::string& canonicalTypeName,
    runtime::RuntimeError& error) const {
    (void)error;
    if (!heap_) return false;
    const auto type = findType(canonicalTypeName);
    if (!type) return false;
    return heap_->removeNativeEcsComponent(
        type->canonicalName(), static_cast<std::int64_t>(entity));
}

bool ScriptRuntime::destroyEcsEntity(
    std::uint64_t entity,
    runtime::RuntimeError& error) const {
    (void)error;
    if (!heap_) return false;
    return heap_->destroyNativeEcsEntity(static_cast<std::int64_t>(entity));
}

void ScriptRuntime::queueEcsSetComponent(
    std::uint64_t entity,
    const std::string& canonicalTypeName,
    runtime::Value component) const {
    ecsCommandBuffer_.push_back(EcsQueuedCommand{
        EcsQueuedCommand::Kind::SetComponent,
        entity,
        canonicalTypeName,
        std::move(component),
    });
}

void ScriptRuntime::queueEcsRemoveComponent(
    std::uint64_t entity,
    const std::string& canonicalTypeName) const {
    ecsCommandBuffer_.push_back(EcsQueuedCommand{
        EcsQueuedCommand::Kind::RemoveComponent,
        entity,
        canonicalTypeName,
        {},
    });
}

void ScriptRuntime::queueEcsDestroyEntity(std::uint64_t entity) const {
    ecsCommandBuffer_.push_back(EcsQueuedCommand{
        EcsQueuedCommand::Kind::DestroyEntity,
        entity,
        {},
        {},
    });
}

std::size_t ScriptRuntime::flushEcsCommandBuffer(runtime::RuntimeError& error) const {
    auto commands = std::move(ecsCommandBuffer_);
    ecsCommandBuffer_.clear();
    std::size_t executed = 0;
    for (const auto& cmd : commands) {
        switch (cmd.kind) {
        case EcsQueuedCommand::Kind::SetComponent:
            if (setEcsEntityComponent(cmd.entity, cmd.canonicalTypeName, cmd.component, error)) {
                ++executed;
            }
            break;
        case EcsQueuedCommand::Kind::RemoveComponent:
            if (removeEcsEntityComponent(cmd.entity, cmd.canonicalTypeName, error)) {
                ++executed;
            }
            break;
        case EcsQueuedCommand::Kind::DestroyEntity:
            if (destroyEcsEntity(cmd.entity, error)) {
                ++executed;
            }
            break;
        }
    }
    return executed;
}

} // namespace realscript::game
