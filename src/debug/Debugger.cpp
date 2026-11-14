#include "realscript/debug/Debugger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_set>
#include <utility>

namespace realscript::debug {
namespace {


std::string normalizeDebugPath(std::string_view p) {
    std::string out;
    out.reserve(p.size());
    for (char ch : p) {
        char norm = (ch == '\\') ? '/' : ch;
#if defined(_WIN32)
        if (norm >= 'A' && norm <= 'Z') {
            norm = static_cast<char>(norm - 'A' + 'a');
        }
#endif
        out.push_back(norm);
    }
#if !defined(_WIN32)
    if (out.size() >= 2 && out[1] == ':' && out[0] >= 'A' && out[0] <= 'Z') {
        out[0] = static_cast<char>(out[0] - 'A' + 'a');
    }
#endif
    return out;
}

bool debugPathsMatch(std::string_view requestedPath, std::string_view enginePath) noexcept {
    if (requestedPath == enginePath) return true;
    const std::string normReq = normalizeDebugPath(requestedPath);
    const std::string normEng = normalizeDebugPath(enginePath);
    if (normReq == normEng) return true;
    if (normReq.size() > normEng.size() &&
        normReq.compare(normReq.size() - normEng.size(), normEng.size(), normEng) == 0 &&
        normReq[normReq.size() - normEng.size() - 1] == '/') {
        return true;
    }
    if (normEng.size() > normReq.size() &&
        normEng.compare(normEng.size() - normReq.size(), normReq.size(), normReq) == 0 &&
        normEng[normEng.size() - normReq.size() - 1] == '/') {
        return true;
    }
    return false;
}

bool sameLocation(const SourceRange& left, const SourceRange& right) noexcept {
    return left.fileId == right.fileId &&
        left.start.line == right.start.line &&
        left.start.column == right.start.column;
}

const SourceFileInfo* sourceFile(
    const bytecode::Module& module,
    SourceFileId id) noexcept {
    for (const auto& source : module.sourceFiles) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

std::string sequencePointSourcePath(
    const bytecode::Module& module,
    const SequencePoint& point) {
    if (!point.mappedPath.empty()) return point.mappedPath;
    if (const auto* source = sourceFile(module, point.range.fileId)) {
        return source->path;
    }
    return {};
}

DebugStackFrame captureSingleFrame(
    const DebugFrameView& view,
    std::uint32_t frameId) {
    DebugStackFrame frame;
    frame.id = frameId;
    if (!view.module || !view.function) return frame;
    const bool sourceLambda = view.function->name.find("$lambda_") == 0;
    frame.function = view.module->name + "::" +
        (sourceLambda ? "<lambda>" : view.function->name);
    if (view.point) {
        frame.location = view.point->range;
        frame.sourcePath = sequencePointSourcePath(*view.module, *view.point);
    }
    for (const auto& local : view.function->debugInfo.locals) {
        if (sourceLambda && local.parameter && local.name == "this") {
            continue;
        }
        const auto& values = view.locals;
        if (!values.values || local.slot >= values.size()) continue;
        if (!local.parameter && view.point) {
            const auto offset = view.point->range.span.start;
            if (offset < local.scope.span.start ||
                offset > local.scope.span.end()) continue;
        }
        DebugVariable variable;
        variable.name = local.name;
        variable.value = values[local.slot];
        variable.type = local.type;
        variable.typeId = local.typeId;
        if (local.parameter) {
            frame.arguments.push_back(std::move(variable));
        } else {
            frame.locals.push_back(std::move(variable));
        }
    }
    return frame;
}

bool tryAsInt64(const runtime::Value& value, std::int64_t& out) noexcept {
    if (const auto* v = std::get_if<std::int64_t>(&value)) { out = *v; return true; }
    if (const auto* v = std::get_if<runtime::LongValue>(&value)) { out = v->value; return true; }
    if (const auto* v = std::get_if<runtime::ByteValue>(&value)) { out = v->value; return true; }
    if (const auto* v = std::get_if<runtime::SByteValue>(&value)) { out = v->value; return true; }
    if (const auto* v = std::get_if<runtime::ShortValue>(&value)) { out = v->value; return true; }
    if (const auto* v = std::get_if<runtime::UShortValue>(&value)) { out = v->value; return true; }
    if (const auto* v = std::get_if<runtime::UIntValue>(&value)) { out = static_cast<std::int64_t>(v->value); return true; }
    if (const auto* v = std::get_if<runtime::ULongValue>(&value)) { out = static_cast<std::int64_t>(v->value); return true; }
    if (const auto* v = std::get_if<runtime::CharValue>(&value)) { out = v->value; return true; }
    if (runtime::EnumValue v{}; value.tryGetEnum(v)) { out = v.value; return true; }
    return false;
}

bool tryAsDouble(const runtime::Value& value, double& out) noexcept {
    if (const auto* v = std::get_if<double>(&value)) { out = *v; return true; }
    if (const auto* v = std::get_if<runtime::FloatValue>(&value)) { out = v->value; return true; }
    if (const auto* v = std::get_if<runtime::FixedValue>(&value)) {
        out = static_cast<double>(v->raw) / 256.0;
        return true;
    }
    if (const auto* v = std::get_if<runtime::Fixed16Value>(&value)) {
        out = static_cast<double>(v->raw) / 65536.0;
        return true;
    }
    if (const auto* v = std::get_if<runtime::Fixed64Value>(&value)) {
        out = static_cast<double>(v->raw) / 4294967296.0;
        return true;
    }
    std::int64_t integer = 0;
    if (tryAsInt64(value, integer)) {
        out = static_cast<double>(integer);
        return true;
    }
    return false;
}

bool tryAsString(
    const runtime::Value& value,
    runtime::ManagedHeap* heap,
    std::string& out) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        out = *text;
        return true;
    }
    if (const auto* sv = std::get_if<runtime::StringValue>(&value)) {
        out = sv->str();
        return true;
    }
    if (const auto* ref = std::get_if<runtime::ObjectRef>(&value)) {
        if (heap && ref->kind == runtime::ObjectKind::String) {
            if (const auto view = heap->stringView(*ref)) {
                out = std::string(*view);
                return true;
            }
        }
    }
    return false;
}

bool isNullValue(const runtime::Value& value) noexcept {
    if (std::holds_alternative<std::monostate>(value) ||
        std::holds_alternative<runtime::NullString>(value) ||
        std::holds_alternative<runtime::NullObject>(value) ||
        std::holds_alternative<runtime::NullArray>(value)) {
        return true;
    }
    if (const auto* ref = std::get_if<runtime::ObjectRef>(&value)) {
        return !ref->valid();
    }
    if (runtime::NativeHandle handle{}; value.tryGetNativeHandle(handle)) {
        return !handle.valid();
    }
    return false;
}

bool valuesEqualForEval(
    const runtime::Value& left,
    const runtime::Value& right,
    runtime::ManagedHeap* heap) {
    if (isNullValue(left) || isNullValue(right)) {
        return isNullValue(left) && isNullValue(right);
    }
    std::int64_t li = 0, ri = 0;
    if (tryAsInt64(left, li) && tryAsInt64(right, ri)) {
        return li == ri;
    }
    double ld = 0.0, rd = 0.0;
    if (tryAsDouble(left, ld) && tryAsDouble(right, rd)) {
        return ld == rd;
    }
    std::string ls, rs;
    if (tryAsString(left, heap, ls) && tryAsString(right, heap, rs)) {
        return ls == rs;
    }
    return left == right;
}

std::optional<runtime::Value> tokenValueToRuntimeValue(const syntax::TokenValue& tv) {
    if (const auto* i = std::get_if<std::int64_t>(&tv)) return runtime::Value{*i};
    if (const auto* d = std::get_if<double>(&tv)) return runtime::Value{*d};
    if (const auto* b = std::get_if<bool>(&tv)) return runtime::Value{*b};
    if (const auto* s = std::get_if<std::string>(&tv)) return runtime::Value{*s};
    return std::nullopt;
}

// Verifies that a bytecode::Function (and any transitive callees up to depth 4)
// performs strictly side-effect-free read-only operations and never mutates heap,
// static fields, arrays, structs, or external native state.
bool isPureBytecodeFunction(
    const runtime::ProgramImage& program,
    const bytecode::Module& module,
    const bytecode::Function& fn,
    std::size_t depth,
    std::unordered_set<semantic::SymbolId>& visited) {
    if (depth > 4) return false;
    if (fn.symbolId != 0 && !visited.insert(fn.symbolId).second) {
        // Recursive cycle: still pure so long as instructions are pure (bounded by instructionBudget)
        return true;
    }
    for (const auto& block : fn.blocks) {
        if (block.terminator.kind == bytecode::TerminatorKind::Throw) {
            return false;
        }
        for (const auto& inst : block.instructions) {
            switch (inst.opcode) {
            case bytecode::Opcode::LoadParameter:
            case bytecode::Opcode::ConstantInt:
            case bytecode::Opcode::ConstantDouble:
            case bytecode::Opcode::ConstantBool:
            case bytecode::Opcode::ConstantString:
            case bytecode::Opcode::ConstantNull:
            case bytecode::Opcode::ConstantTypeId:
            case bytecode::Opcode::LoadLocal:
            case bytecode::Opcode::StoreLocal:
            case bytecode::Opcode::ConvertNullToString:
            case bytecode::Opcode::ConvertNullToObject:
            case bytecode::Opcode::ConvertNullToArray:
            case bytecode::Opcode::ConvertIntToLong:
            case bytecode::Opcode::ConvertIntToDouble:
            case bytecode::Opcode::ConvertLongToDouble:
            case bytecode::Opcode::ConvertNumeric:
            case bytecode::Opcode::NewStruct:
            case bytecode::Opcode::CheckNotNull:
            case bytecode::Opcode::ArrayLength:
            case bytecode::Opcode::LoadElement:
            case bytecode::Opcode::LoadField:
            case bytecode::Opcode::LoadStructField:
            case bytecode::Opcode::NegateInt:
            case bytecode::Opcode::NegateLong:
            case bytecode::Opcode::NegateDouble:
            case bytecode::Opcode::LogicalNot:
            case bytecode::Opcode::AddInt:
            case bytecode::Opcode::SubtractInt:
            case bytecode::Opcode::MultiplyInt:
            case bytecode::Opcode::DivideInt:
            case bytecode::Opcode::RemainderInt:
            case bytecode::Opcode::Equal:
            case bytecode::Opcode::NotEqual:
            case bytecode::Opcode::LessInt:
            case bytecode::Opcode::LessOrEqualInt:
            case bytecode::Opcode::GreaterInt:
            case bytecode::Opcode::GreaterOrEqualInt:
            case bytecode::Opcode::AddLong:
            case bytecode::Opcode::SubtractLong:
            case bytecode::Opcode::MultiplyLong:
            case bytecode::Opcode::DivideLong:
            case bytecode::Opcode::RemainderLong:
            case bytecode::Opcode::LessLong:
            case bytecode::Opcode::LessOrEqualLong:
            case bytecode::Opcode::GreaterLong:
            case bytecode::Opcode::GreaterOrEqualLong:
            case bytecode::Opcode::AddDouble:
            case bytecode::Opcode::SubtractDouble:
            case bytecode::Opcode::MultiplyDouble:
            case bytecode::Opcode::DivideDouble:
            case bytecode::Opcode::LessDouble:
            case bytecode::Opcode::LessOrEqualDouble:
            case bytecode::Opcode::GreaterDouble:
            case bytecode::Opcode::GreaterOrEqualDouble:
            case bytecode::Opcode::IsType:
            case bytecode::Opcode::AsType:
                break;

            case bytecode::Opcode::Call: {
                if (inst.index >= module.functionReferences.size()) return false;
                const auto& fnRef = module.functionReferences[inst.index];
                if (fnRef.virtualDispatch || fnRef.interfaceDispatch) return false;
                if (fnRef.name.rfind("RealScript.Math::", 0) == 0 ||
                    fnRef.name.rfind("$intrinsic::static_get::", 0) == 0 ||
                    fnRef.name.rfind("$intrinsic::nullable_", 0) == 0 ||
                    fnRef.name.rfind("$intrinsic::string_", 0) == 0) {
                    break;
                }
                const auto* targetLoc = program.resolveFunction(fnRef.symbolId);
                if (!targetLoc || !targetLoc->module || !targetLoc->function) {
                    return false; // External C++ host binding or unresolved
                }
                if (!isPureBytecodeFunction(
                        program, *targetLoc->module, *targetLoc->function, depth + 1, visited)) {
                    return false;
                }
                break;
            }

            // Mutating, heap-allocating, delegate, or async opcodes are rejected
            case bytecode::Opcode::NewObject:
            case bytecode::Opcode::NewArray:
            case bytecode::Opcode::StoreElement:
            case bytecode::Opcode::StoreField:
            case bytecode::Opcode::StoreStructField:
            case bytecode::Opcode::NewDelegate:
            case bytecode::Opcode::InvokeDelegate:
            case bytecode::Opcode::CombineDelegate:
            case bytecode::Opcode::RemoveDelegate:
            case bytecode::Opcode::AsyncEnter:
            case bytecode::Opcode::AwaitSuspend:
            case bytecode::Opcode::AwaitResume:
            case bytecode::Opcode::AsyncResolve:
                return false;
            }
        }
    }
    return true;
}

class ExpressionEvaluator {
public:
    ExpressionEvaluator(
        const std::string& text,
        const DebugStackFrame& frame,
        const std::unordered_map<semantic::SymbolId, semantic::TypeSymbol>& types,
        const std::unordered_map<std::string, semantic::SymbolId>& typesByName,
        const runtime::ProgramImage* program,
        runtime::ManagedHeap* heap)
        : text_(text),
          frame_(frame),
          types_(types),
          typesByName_(typesByName),
          program_(program),
          heap_(heap) {}

    std::optional<runtime::Value> evaluate(std::string* errorOut) {
        auto result = parseConditional();
        skipWhitespace();
        if (result && pos_ < text_.size()) {
            setError("unexpected trailing token in expression");
            result.reset();
        }
        if (!result && errorOut) {
            *errorOut = error_.empty() ? "invalid expression" : error_;
        }
        return result;
    }

private:
    struct ScopedSuppress {
        bool& flag;
        bool prev;
        ScopedSuppress(bool& f, bool enable) noexcept : flag(f), prev(f) {
            if (enable) flag = true;
        }
        ~ScopedSuppress() noexcept { flag = prev; }
    };

    void setError(std::string message) {
        if (!suppressEval_ && error_.empty()) error_ = std::move(message);
    }

    void skipWhitespace() noexcept {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool match(std::string_view token) noexcept {
        skipWhitespace();
        if (text_.compare(pos_, token.size(), token) != 0) return false;
        pos_ += token.size();
        return true;
    }

    bool peek(std::string_view token) noexcept {
        skipWhitespace();
        return text_.compare(pos_, token.size(), token) == 0;
    }

    const semantic::TypeSymbol* findBaseType(const semantic::TypeSymbol& type) const noexcept {
        if (type.baseTypeId != 0) {
            const auto it = types_.find(type.baseTypeId);
            if (it != types_.end()) return &it->second;
        }
        if (!type.baseTypeName.empty()) {
            const auto itName = typesByName_.find(type.baseTypeName);
            if (itName != typesByName_.end()) {
                const auto it = types_.find(itName->second);
                if (it != types_.end()) return &it->second;
            }
        }
        return nullptr;
    }

    std::optional<runtime::Value> invokePureSandbox(
        semantic::SymbolId fnSymbolId,
        const std::vector<runtime::Value>& callArgs,
        const std::string& displayLabel) {
        if (!program_) {
            setError("program image is unavailable to evaluate '" + displayLabel + "'");
            return std::nullopt;
        }
        const auto* loc = program_->resolveFunction(fnSymbolId);
        if (!loc || !loc->module || !loc->function) {
            setError("cannot evaluate external or native function '" + displayLabel + "' in watch sandbox");
            return std::nullopt;
        }
        std::unordered_set<semantic::SymbolId> visited;
        if (!isPureBytecodeFunction(*program_, *loc->module, *loc->function, 0, visited)) {
            setError("refusing to evaluate '" + displayLabel + "': function has potential side effects");
            return std::nullopt;
        }
        // Aliasing shared_ptrs with empty control block: safe non-owning view for synchronous sandbox call
        std::shared_ptr<const runtime::ProgramImage> progAlias(
            std::shared_ptr<const runtime::ProgramImage>{}, program_);
        std::shared_ptr<runtime::ManagedHeap> heapAlias = heap_
            ? std::shared_ptr<runtime::ManagedHeap>(std::shared_ptr<runtime::ManagedHeap>{}, heap_)
            : std::shared_ptr<runtime::ManagedHeap>{};
        runtime::Interpreter sandbox(std::move(progAlias), std::move(heapAlias));
        runtime::ExecutionOptions opts;
        opts.debugger = nullptr; // Never trigger breakpoints or re-enter DebugController!
        opts.limits.instructionBudget = 2048;
        opts.limits.recursionLimit = 16;
        auto execRes = sandbox.invoke(fnSymbolId, callArgs, std::move(opts));
        if (!execRes.succeeded) {
            setError("evaluation of '" + displayLabel + "' failed: " + execRes.error.message);
            return std::nullopt;
        }
        return execRes.value;
    }

    std::optional<runtime::Value> getNamedObjectField(
        runtime::ObjectRef ref,
        const semantic::TypeSymbol& typeDesc,
        std::string_view fieldName,
        std::size_t fallbackIdx) const {
        if (!heap_) return std::nullopt;
        for (std::size_t i = 0; i < typeDesc.fields.size(); ++i) {
            if (typeDesc.fields[i].name == fieldName) {
                return heap_->fieldGet(ref, i);
            }
        }
        return heap_->fieldGet(ref, fallbackIdx);
    }

    bool isNullableNull(const runtime::Value& val) const {
        const auto tid = runtime::structValueTypeId(val);
        if (!tid || *tid == 0) return false;
        const auto it = types_.find(*tid);
        if (it == types_.end() || it->second.name.rfind("Nullable__", 0) != 0) return false;
        std::optional<runtime::Value> hv;
        if (const auto* sv = std::get_if<runtime::StructValue>(&val)) {
            if (sv->valid()) hv = runtime::structValueFieldAt(*sv, 0);
        } else {
            hv = runtime::loadInlineStructField(val, 0);
        }
        if (hv) {
            if (const auto* b = std::get_if<bool>(&*hv)) return !*b;
        }
        return false;
    }

    std::optional<runtime::Value> unwrapNullableIfPresent(const runtime::Value& val) const {
        const auto tid = runtime::structValueTypeId(val);
        if (!tid || *tid == 0) return val;
        const auto it = types_.find(*tid);
        if (it == types_.end() || it->second.name.rfind("Nullable__", 0) != 0) return val;
        std::optional<runtime::Value> hv, inner;
        if (const auto* sv = std::get_if<runtime::StructValue>(&val)) {
            if (sv->valid()) {
                hv = runtime::structValueFieldAt(*sv, 0);
                inner = runtime::structValueFieldAt(*sv, 1);
            }
        } else {
            hv = runtime::loadInlineStructField(val, 0);
            inner = runtime::loadInlineStructField(val, 1);
        }
        if (hv && std::holds_alternative<bool>(*hv) && std::get<bool>(*hv) && inner) {
            return *inner;
        }
        return std::nullopt;
    }

    std::optional<runtime::Value> resolveStaticMember(
        semantic::SymbolId typeId,
        const std::string& member,
        const std::vector<runtime::Value>* callArgs) {
        const auto found = types_.find(typeId);
        if (found == types_.end()) {
            setError("unknown type metadata");
            return std::nullopt;
        }
        std::unordered_set<semantic::SymbolId> visited;
        const semantic::TypeSymbol* curr = &found->second;
        while (curr && visited.insert(curr->id).second) {
            if (!callArgs) {
                for (const auto& em : curr->enumMembers) {
                    if (em.name == member) {
                        return runtime::Value{runtime::EnumValue{curr->id, em.value}};
                    }
                }
                for (const auto& field : curr->fields) {
                    if (field.name != member) continue;
                    if (field.constField && field.hasConstantValue) {
                        if (auto cv = tokenValueToRuntimeValue(field.constantValue)) return *cv;
                    }
                    if (field.staticField) {
                        if (heap_ && heap_->hasStaticField(field.id)) {
                            if (auto sv = heap_->getStaticField(field.id)) return *sv;
                        }
                        if (field.hasConstantValue) {
                            if (auto cv = tokenValueToRuntimeValue(field.constantValue)) return *cv;
                        }
                        return runtime::Value{std::int64_t{0}};
                    }
                }
                for (const auto& prop : curr->properties) {
                    if (prop.name == member && prop.staticProperty) {
                        if (prop.backingFieldIndex < curr->fields.size()) {
                            const auto& bf = curr->fields[prop.backingFieldIndex];
                            if (heap_ && heap_->hasStaticField(bf.id)) {
                                if (auto sv = heap_->getStaticField(bf.id)) return *sv;
                            }
                        }
                        if (prop.getter) {
                            return invokePureSandbox(prop.getter->id, {}, curr->name + "." + member);
                        }
                    }
                }
            } else {
                for (const auto& method : curr->methods) {
                    if (method.name == member && method.staticMethod &&
                        method.parameters.size() == callArgs->size()) {
                        return invokePureSandbox(method.id, *callArgs, curr->name + "." + member);
                    }
                }
            }
            curr = findBaseType(*curr);
        }
        setError("unknown static member '" + member + "' on '" + found->second.name + "'");
        return std::nullopt;
    }

    std::optional<runtime::Value> resolveMemberOrCall(
        const runtime::Value& target,
        const std::string& member,
        const std::vector<runtime::Value>* callArgs) {
        // 1. Inline Math & Fixed primitives (supports both inline cells and StructValue wrappers)
        if (std::int32_t fxRaw = 0; target.tryGetFixed(fxRaw)) {
            if (!callArgs && (member == "raw" || member == "Raw")) return runtime::Value{static_cast<std::int64_t>(fxRaw)};
            if (callArgs && callArgs->empty() && member == "ToDouble") return runtime::Value{static_cast<double>(fxRaw) / 256.0};
            if (callArgs && callArgs->empty() && member == "ToInt") return runtime::Value{static_cast<std::int64_t>(fxRaw / 256)};
        }
        if (const auto* fx16 = std::get_if<runtime::Fixed16Value>(&target)) {
            if (!callArgs && (member == "raw" || member == "Raw")) return runtime::Value{static_cast<std::int64_t>(fx16->raw)};
            if (callArgs && callArgs->empty() && member == "ToDouble") return runtime::Value{static_cast<double>(fx16->raw) / 65536.0};
            if (callArgs && callArgs->empty() && member == "ToInt") return runtime::Value{static_cast<std::int64_t>(fx16->raw / 65536)};
        }
        if (runtime::Vec2Value v2{}; target.tryGetVec2(v2)) {
            if (!callArgs) {
                if (member == "x") return runtime::Value{runtime::FixedValue{v2.x}};
                if (member == "y") return runtime::Value{runtime::FixedValue{v2.y}};
            } else if (callArgs->empty() && member == "LengthSquared") {
                const std::int64_t xx = (static_cast<std::int64_t>(v2.x) * v2.x) >> 8;
                const std::int64_t yy = (static_cast<std::int64_t>(v2.y) * v2.y) >> 8;
                return runtime::Value{runtime::FixedValue{static_cast<std::int32_t>(xx + yy)}};
            }
        }
        if (runtime::Vec3Value v3{}; target.tryGetVec3(v3)) {
            if (!callArgs) {
                if (member == "x") return runtime::Value{runtime::FixedValue{v3.x}};
                if (member == "y") return runtime::Value{runtime::FixedValue{v3.y}};
                if (member == "z") return runtime::Value{runtime::FixedValue{v3.z}};
            } else if (callArgs->empty() && member == "LengthSquared") {
                const std::int64_t xx = (static_cast<std::int64_t>(v3.x) * v3.x) >> 8;
                const std::int64_t yy = (static_cast<std::int64_t>(v3.y) * v3.y) >> 8;
                const std::int64_t zz = (static_cast<std::int64_t>(v3.z) * v3.z) >> 8;
                return runtime::Value{runtime::FixedValue{static_cast<std::int32_t>(xx + yy + zz)}};
            }
        }
        if (runtime::QuatValue q{}; target.tryGetQuat(q)) {
            if (!callArgs) {
                if (member == "x") return runtime::Value{runtime::FixedValue{q.x}};
                if (member == "y") return runtime::Value{runtime::FixedValue{q.y}};
                if (member == "z") return runtime::Value{runtime::FixedValue{q.z}};
                if (member == "w") return runtime::Value{runtime::FixedValue{q.w}};
            }
        }

        // 2. Value-type structs (including Nullable<T>)
        const auto structTypeId = runtime::structValueTypeId(target);
        if (structTypeId && *structTypeId != 0) {
            const auto foundType = types_.find(*structTypeId);
            if (foundType == types_.end()) {
                setError("unknown struct type metadata");
                return std::nullopt;
            }
            const auto readStructSlot = [&](std::size_t idx) -> std::optional<runtime::Value> {
                if (const auto* sv = std::get_if<runtime::StructValue>(&target)) {
                    if (!sv->valid()) return std::nullopt;
                    return runtime::structValueFieldAt(*sv, idx);
                }
                return runtime::loadInlineStructField(target, idx);
            };
            if (foundType->second.name.rfind("Nullable__", 0) == 0) {
                const auto hv = readStructSlot(0);
                const bool hasVal = hv && std::holds_alternative<bool>(*hv) && std::get<bool>(*hv);
                if (!callArgs) {
                    if (member == "HasValue" || member == "hasValue") return runtime::Value{hasVal};
                    if (member == "Value" || member == "value") {
                        if (!hasVal) {
                            setError("Nullable value has no value");
                            return std::nullopt;
                        }
                        return readStructSlot(1);
                    }
                } else if (member == "GetValueOrDefault") {
                    if (callArgs->empty()) {
                        return readStructSlot(1);
                    }
                    if (callArgs->size() == 1) {
                        return hasVal ? readStructSlot(1) : (*callArgs)[0];
                    }
                }
            }
            if (!callArgs) {
                for (std::size_t index = 0; index < foundType->second.fields.size(); ++index) {
                    if (foundType->second.fields[index].name == member) {
                        if (auto fieldVal = readStructSlot(index)) return *fieldVal;
                    }
                }
                for (const auto& prop : foundType->second.properties) {
                    if (prop.name == member && !prop.staticProperty) {
                        if (prop.backingFieldIndex < foundType->second.fields.size()) {
                            if (auto bfVal = readStructSlot(prop.backingFieldIndex)) return *bfVal;
                        }
                        if (prop.getter) {
                            return invokePureSandbox(
                                prop.getter->id, {target}, foundType->second.name + "." + member);
                        }
                    }
                }
                for (const auto& method : foundType->second.methods) {
                    if (method.name == "get_" + member && !method.staticMethod && method.parameters.size() == 1) {
                        return invokePureSandbox(method.id, {target}, foundType->second.name + "." + member);
                    }
                }
            } else {
                std::vector<runtime::Value> fullArgs;
                fullArgs.reserve(callArgs->size() + 1);
                fullArgs.push_back(target);
                fullArgs.insert(fullArgs.end(), callArgs->begin(), callArgs->end());
                for (const auto& method : foundType->second.methods) {
                    if (method.name == member && !method.staticMethod &&
                        method.parameters.size() == fullArgs.size()) {
                        return invokePureSandbox(method.id, fullArgs, foundType->second.name + "." + member);
                    }
                }
            }
            setError("unknown struct member '" + member + "'");
            return std::nullopt;
        }

        // 3. String intrinsics (value strings & heap strings)
        std::string strValue;
        if (tryAsString(target, heap_, strValue)) {
            if (!callArgs || callArgs->empty()) {
                if (member == "Length" || member == "Count" || member == "length") {
                    return runtime::Value{static_cast<std::int64_t>(strValue.size())};
                }
                if (member == "IsEmpty") {
                    return runtime::Value{strValue.empty()};
                }
            }
            if (callArgs && callArgs->size() == 1) {
                std::string argStr;
                if (tryAsString((*callArgs)[0], heap_, argStr)) {
                    if (member == "Contains") {
                        return runtime::Value{strValue.find(argStr) != std::string::npos};
                    }
                    if (member == "StartsWith") {
                        return runtime::Value{strValue.size() >= argStr.size() &&
                            strValue.compare(0, argStr.size(), argStr) == 0};
                    }
                    if (member == "EndsWith") {
                        return runtime::Value{strValue.size() >= argStr.size() &&
                            strValue.compare(strValue.size() - argStr.size(), argStr.size(), argStr) == 0};
                    }
                }
            }
        }

        // 4. Managed Heap Objects & Arrays (including Monomorphized Built-in Containers)
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&target)) {
            if (!ref->valid()) {
                setError("null object reference");
                return std::nullopt;
            }
            if (!heap_) {
                setError("managed heap is unavailable for object inspection");
                return std::nullopt;
            }
            if (ref->kind == runtime::ObjectKind::Array) {
                if (const auto len = heap_->arrayLength(*ref)) {
                    if (!callArgs || callArgs->empty()) {
                        if (member == "Length" || member == "Count" || member == "length") {
                            return runtime::Value{static_cast<std::int64_t>(*len)};
                        }
                        if (member == "IsEmpty") {
                            return runtime::Value{*len == 0};
                        }
                    }
                }
                setError("unknown array member '" + member + "'");
                return std::nullopt;
            }
            const auto typeId = heap_->objectTypeId(*ref);
            if (!typeId) {
                setError("object reference is no longer alive");
                return std::nullopt;
            }
            const auto foundType = types_.find(*typeId);
            if (foundType == types_.end()) {
                setError("unknown object type metadata");
                return std::nullopt;
            }
            const auto& tDesc = foundType->second;
            const std::string& tName = tDesc.name;

            // Built-in container logical properties & side-effect-free queries
            const bool isList = (tName.rfind("List__", 0) == 0);
            const bool isHashSet = (tName.rfind("HashSet__", 0) == 0);
            const bool isStack = (tName.rfind("Stack__", 0) == 0);
            const bool isQueue = (tName.rfind("Queue__", 0) == 0);
            const bool isDict = (tName.rfind("Dictionary__", 0) == 0);
            if (isList || isHashSet || isStack || isQueue || isDict) {
                const auto fCount = heap_->fieldCount(*ref).value_or(0);
                const std::size_t countFallback = isHashSet ? (fCount > 0 ? fCount - 1 : 0)
                    : (isQueue ? 3 : (isDict ? 4 : 1));
                const auto countVal = getNamedObjectField(*ref, tDesc, "count", countFallback);
                const auto backingVal = isHashSet
                    ? getNamedObjectField(*ref, tDesc, "values", 1)
                    : (isDict ? getNamedObjectField(*ref, tDesc, "keys", 0)
                              : getNamedObjectField(*ref, tDesc, "items", 0));
                std::int64_t logicalCount = 0;
                if (countVal) (void)tryAsInt64(*countVal, logicalCount);
                if (logicalCount < 0) logicalCount = 0;

                if (!callArgs || callArgs->empty()) {
                    if (member == "Count") return runtime::Value{logicalCount};
                    if (member == "IsEmpty") return runtime::Value{logicalCount == 0};
                    if (member == "Capacity") {
                        std::int64_t cap = 0;
                        if (backingVal) {
                            if (const auto* bRef = std::get_if<runtime::ObjectRef>(&*backingVal);
                                bRef && bRef->valid() && bRef->kind == runtime::ObjectKind::Array) {
                                cap = static_cast<std::int64_t>(heap_->arrayLength(*bRef).value_or(0));
                            }
                        }
                        return runtime::Value{cap};
                    }
                    if (member == "Peek" && callArgs && callArgs->empty()) {
                        if (logicalCount <= 0) {
                            setError("cannot Peek an empty container");
                            return std::nullopt;
                        }
                        if (isStack && backingVal) {
                            if (const auto* bRef = std::get_if<runtime::ObjectRef>(&*backingVal)) {
                                return heap_->arrayGet(*bRef, static_cast<std::size_t>(logicalCount - 1));
                            }
                        }
                        if (isQueue && backingVal) {
                            const auto headVal = getNamedObjectField(*ref, tDesc, "head", 1);
                            std::int64_t headIdx = 0;
                            if (headVal) (void)tryAsInt64(*headVal, headIdx);
                            if (const auto* bRef = std::get_if<runtime::ObjectRef>(&*backingVal)) {
                                const auto arrLen = heap_->arrayLength(*bRef).value_or(0);
                                if (arrLen > 0) {
                                    return heap_->arrayGet(*bRef, static_cast<std::size_t>(headIdx) % arrLen);
                                }
                            }
                        }
                    }
                }
                if (callArgs && callArgs->size() == 1) {
                    const auto& queryArg = (*callArgs)[0];
                    if ((isList || isHashSet || isStack) && member == "Contains") {
                        bool found = false;
                        if (backingVal) {
                            if (const auto* bRef = std::get_if<runtime::ObjectRef>(&*backingVal);
                                bRef && bRef->valid()) {
                                for (std::int64_t i = 0; i < logicalCount; ++i) {
                                    if (const auto elem = heap_->arrayGet(*bRef, static_cast<std::size_t>(i));
                                        elem && valuesEqualForEval(*elem, queryArg, heap_)) {
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                        return runtime::Value{found};
                    }
                    if (isQueue && member == "Contains") {
                        bool found = false;
                        const auto headVal = getNamedObjectField(*ref, tDesc, "head", 1);
                        std::int64_t headIdx = 0;
                        if (headVal) (void)tryAsInt64(*headVal, headIdx);
                        if (backingVal) {
                            if (const auto* bRef = std::get_if<runtime::ObjectRef>(&*backingVal);
                                bRef && bRef->valid()) {
                                const auto arrLen = heap_->arrayLength(*bRef).value_or(0);
                                if (arrLen > 0) {
                                    for (std::int64_t i = 0; i < logicalCount; ++i) {
                                        const std::size_t phys =
                                            (static_cast<std::size_t>(headIdx) + static_cast<std::size_t>(i)) % arrLen;
                                        if (const auto elem = heap_->arrayGet(*bRef, phys);
                                            elem && valuesEqualForEval(*elem, queryArg, heap_)) {
                                            found = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        return runtime::Value{found};
                    }
                    if (isDict && (member == "ContainsKey" || member == "ContainsValue")) {
                        const auto arrVal = (member == "ContainsKey")
                            ? getNamedObjectField(*ref, tDesc, "keys", 0)
                            : getNamedObjectField(*ref, tDesc, "values", 1);
                        bool found = false;
                        if (arrVal) {
                            if (const auto* bRef = std::get_if<runtime::ObjectRef>(&*arrVal);
                                bRef && bRef->valid()) {
                                for (std::int64_t i = 0; i < logicalCount; ++i) {
                                    if (const auto elem = heap_->arrayGet(*bRef, static_cast<std::size_t>(i));
                                        elem && valuesEqualForEval(*elem, queryArg, heap_)) {
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                        return runtime::Value{found};
                    }
                }
            }

            // General class hierarchy lookup (fields -> auto-properties -> pure getters -> pure methods)
            if (!callArgs) {
                for (std::size_t index = 0; index < tDesc.fields.size(); ++index) {
                    const auto& fld = tDesc.fields[index];
                    if (fld.name != member) continue;
                    if (fld.constField && fld.hasConstantValue) {
                        if (auto cv = tokenValueToRuntimeValue(fld.constantValue)) return *cv;
                    }
                    if (fld.staticField) {
                        if (heap_->hasStaticField(fld.id)) {
                            if (auto sv = heap_->getStaticField(fld.id)) return *sv;
                        }
                        return runtime::Value{std::int64_t{0}};
                    }
                    if (const auto fieldVal = heap_->fieldGet(*ref, index)) {
                        return *fieldVal;
                    }
                }
                std::unordered_set<semantic::SymbolId> visited;
                const semantic::TypeSymbol* curr = &tDesc;
                while (curr && visited.insert(curr->id).second) {
                    for (const auto& prop : curr->properties) {
                        if (prop.name != member) continue;
                        if (!prop.staticProperty && prop.backingFieldIndex < tDesc.fields.size()) {
                            if (const auto bfVal = heap_->fieldGet(*ref, prop.backingFieldIndex)) {
                                return *bfVal;
                            }
                        }
                        if (prop.getter) {
                            if (prop.staticProperty) {
                                return invokePureSandbox(prop.getter->id, {}, curr->name + "." + member);
                            }
                            return invokePureSandbox(prop.getter->id, {target}, curr->name + "." + member);
                        }
                    }
                    for (const auto& method : curr->methods) {
                        if (method.name == "get_" + member && !method.staticMethod &&
                            method.parameters.size() == 1) {
                            return invokePureSandbox(method.id, {target}, curr->name + "." + member);
                        }
                    }
                    for (const auto& fld : curr->fields) {
                        if (fld.name != member) continue;
                        if (fld.constField && fld.hasConstantValue) {
                            if (auto cv = tokenValueToRuntimeValue(fld.constantValue)) return *cv;
                        }
                        if (fld.staticField && heap_->hasStaticField(fld.id)) {
                            if (auto sv = heap_->getStaticField(fld.id)) return *sv;
                        }
                    }
                    curr = findBaseType(*curr);
                }
                setError("unknown object field or property '" + member + "'");
                return std::nullopt;
            } else {
                std::vector<runtime::Value> fullArgs;
                fullArgs.reserve(callArgs->size() + 1);
                fullArgs.push_back(target);
                fullArgs.insert(fullArgs.end(), callArgs->begin(), callArgs->end());

                std::unordered_set<semantic::SymbolId> visited;
                const semantic::TypeSymbol* curr = &tDesc;
                while (curr && visited.insert(curr->id).second) {
                    for (const auto& method : curr->methods) {
                        if (method.name == member && !method.staticMethod &&
                            method.parameters.size() == fullArgs.size()) {
                            return invokePureSandbox(method.id, fullArgs, curr->name + "." + member);
                        }
                        if (method.name == member && method.staticMethod &&
                            method.parameters.size() == callArgs->size()) {
                            return invokePureSandbox(method.id, *callArgs, curr->name + "." + member);
                        }
                    }
                    curr = findBaseType(*curr);
                }
                setError("unknown or unsupported method '" + member + "' on '" + tDesc.name + "'");
                return std::nullopt;
            }
        }

        if (isNullValue(target)) {
            setError("null object reference");
            return std::nullopt;
        }
        setError("cannot access member '" + member + "' on value");
        return std::nullopt;
    }

    std::optional<runtime::Value> resolveIndex(
        const runtime::Value& target,
        const runtime::Value& keyVal) {
        // 1. Inline Math vectors & quaternions (both inline cells and StructValue wrappers)
        if (runtime::Vec2Value v2{}; target.tryGetVec2(v2)) {
            std::int64_t idx = 0;
            if (!tryAsInt64(keyVal, idx) || idx < 0 || idx >= 2) {
                setError("Vec2 index out of range");
                return std::nullopt;
            }
            return runtime::Value{runtime::FixedValue{idx == 0 ? v2.x : v2.y}};
        }
        if (runtime::Vec3Value v3{}; target.tryGetVec3(v3)) {
            std::int64_t idx = 0;
            if (!tryAsInt64(keyVal, idx) || idx < 0 || idx >= 3) {
                setError("Vec3 index out of range");
                return std::nullopt;
            }
            const std::int32_t raw = (idx == 0) ? v3.x : ((idx == 1) ? v3.y : v3.z);
            return runtime::Value{runtime::FixedValue{raw}};
        }
        if (runtime::QuatValue q{}; target.tryGetQuat(q)) {
            std::int64_t idx = 0;
            if (!tryAsInt64(keyVal, idx) || idx < 0 || idx >= 4) {
                setError("Quat index out of range");
                return std::nullopt;
            }
            std::int32_t raw = q.w;
            if (idx == 0) raw = q.x;
            else if (idx == 1) raw = q.y;
            else if (idx == 2) raw = q.z;
            return runtime::Value{runtime::FixedValue{raw}};
        }

        // 2. String character indexing
        std::string strVal;
        if (tryAsString(target, heap_, strVal)) {
            std::int64_t idx = 0;
            if (!tryAsInt64(keyVal, idx) || idx < 0 || static_cast<std::size_t>(idx) >= strVal.size()) {
                setError("string index out of range");
                return std::nullopt;
            }
            return runtime::Value{std::string(1, strVal[static_cast<std::size_t>(idx)])};
        }

        // 3. Struct custom indexer get_Item
        if (const auto structTid = runtime::structValueTypeId(target); structTid && *structTid != 0) {
            std::vector<runtime::Value> args{keyVal};
            return resolveMemberOrCall(target, "get_Item", &args);
        }

        // 4. Managed Arrays & Containers & Class Indexers
        const auto* ref = std::get_if<runtime::ObjectRef>(&target);
        if (!ref || !ref->valid() || !heap_) {
            setError("indexed value is not a valid array or collection");
            return std::nullopt;
        }
        if (ref->kind == runtime::ObjectKind::Array) {
            std::int64_t idx = 0;
            if (!tryAsInt64(keyVal, idx) || idx < 0) {
                setError("array index must be a non-negative integer");
                return std::nullopt;
            }
            const auto elem = heap_->arrayGet(*ref, static_cast<std::size_t>(idx));
            if (!elem) {
                setError("array index out of range");
                return std::nullopt;
            }
            return *elem;
        }

        const auto typeId = heap_->objectTypeId(*ref);
        if (!typeId) {
            setError("indexed object is no longer alive");
            return std::nullopt;
        }
        const auto foundType = types_.find(*typeId);
        if (foundType == types_.end()) {
            setError("unknown indexed object type metadata");
            return std::nullopt;
        }
        const auto& tDesc = foundType->second;
        const std::string& tName = tDesc.name;

        if (tName.rfind("List__", 0) == 0) {
            std::int64_t idx = 0;
            if (!tryAsInt64(keyVal, idx) || idx < 0) {
                setError("List index must be a non-negative integer");
                return std::nullopt;
            }
            const auto itemsVal = getNamedObjectField(*ref, tDesc, "items", 0);
            const auto countVal = getNamedObjectField(*ref, tDesc, "count", 1);
            std::int64_t count = 0;
            if (countVal) (void)tryAsInt64(*countVal, count);
            if (idx >= count || !itemsVal) {
                setError("List index out of range");
                return std::nullopt;
            }
            if (const auto* arrRef = std::get_if<runtime::ObjectRef>(&*itemsVal);
                arrRef && arrRef->valid()) {
                if (const auto elem = heap_->arrayGet(*arrRef, static_cast<std::size_t>(idx))) {
                    return *elem;
                }
            }
            setError("List backing storage unavailable");
            return std::nullopt;
        }

        if (tName.rfind("Stack__", 0) == 0) {
            std::int64_t idx = 0;
            if (!tryAsInt64(keyVal, idx) || idx < 0) {
                setError("Stack index must be a non-negative integer");
                return std::nullopt;
            }
            const auto itemsVal = getNamedObjectField(*ref, tDesc, "items", 0);
            const auto countVal = getNamedObjectField(*ref, tDesc, "count", 1);
            std::int64_t count = 0;
            if (countVal) (void)tryAsInt64(*countVal, count);
            if (idx >= count || !itemsVal) {
                setError("Stack index out of range");
                return std::nullopt;
            }
            const std::size_t physIdx = static_cast<std::size_t>(count - 1 - idx);
            if (const auto* arrRef = std::get_if<runtime::ObjectRef>(&*itemsVal);
                arrRef && arrRef->valid()) {
                if (const auto elem = heap_->arrayGet(*arrRef, physIdx)) {
                    return *elem;
                }
            }
            setError("Stack backing storage unavailable");
            return std::nullopt;
        }

        if (tName.rfind("Queue__", 0) == 0) {
            std::int64_t idx = 0;
            if (!tryAsInt64(keyVal, idx) || idx < 0) {
                setError("Queue index must be a non-negative integer");
                return std::nullopt;
            }
            const auto itemsVal = getNamedObjectField(*ref, tDesc, "items", 0);
            const auto headVal = getNamedObjectField(*ref, tDesc, "head", 1);
            const auto countVal = getNamedObjectField(*ref, tDesc, "count", 3);
            std::int64_t count = 0, head = 0;
            if (countVal) (void)tryAsInt64(*countVal, count);
            if (headVal) (void)tryAsInt64(*headVal, head);
            if (idx >= count || !itemsVal) {
                setError("Queue index out of range");
                return std::nullopt;
            }
            if (const auto* arrRef = std::get_if<runtime::ObjectRef>(&*itemsVal);
                arrRef && arrRef->valid()) {
                const auto arrLen = heap_->arrayLength(*arrRef).value_or(0);
                if (arrLen > 0) {
                    const std::size_t physIdx =
                        (static_cast<std::size_t>(head) + static_cast<std::size_t>(idx)) % arrLen;
                    if (const auto elem = heap_->arrayGet(*arrRef, physIdx)) {
                        return *elem;
                    }
                }
            }
            setError("Queue backing storage unavailable");
            return std::nullopt;
        }

        if (tName.rfind("Dictionary__", 0) == 0) {
            const auto keysVal = getNamedObjectField(*ref, tDesc, "keys", 0);
            const auto valsVal = getNamedObjectField(*ref, tDesc, "values", 1);
            const auto countVal = getNamedObjectField(*ref, tDesc, "count", 4);
            std::int64_t count = 0;
            if (countVal) (void)tryAsInt64(*countVal, count);
            if (keysVal && valsVal) {
                const auto* kRef = std::get_if<runtime::ObjectRef>(&*keysVal);
                const auto* vRef = std::get_if<runtime::ObjectRef>(&*valsVal);
                if (kRef && kRef->valid() && vRef && vRef->valid()) {
                    for (std::int64_t i = 0; i < count; ++i) {
                        const auto kElem = heap_->arrayGet(*kRef, static_cast<std::size_t>(i));
                        if (kElem && valuesEqualForEval(*kElem, keyVal, heap_)) {
                            if (const auto vElem = heap_->arrayGet(*vRef, static_cast<std::size_t>(i))) {
                                return *vElem;
                            }
                        }
                    }
                }
            }
            setError("key not found in Dictionary");
            return std::nullopt;
        }

        // Custom class indexer `this[key]` lowered to `get_Item(key)`
        std::vector<runtime::Value> args{keyVal};
        return resolveMemberOrCall(target, "get_Item", &args);
    }

    std::optional<runtime::Value> parseConditional() {
        auto cond = parseNullCoalescing();
        if (!cond) return std::nullopt;
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '?' &&
            (pos_ + 1 >= text_.size() ||
             (text_[pos_ + 1] != '?' && text_[pos_ + 1] != '.' && text_[pos_ + 1] != '['))) {
            ++pos_; // consume '?'
            bool condTrue = false;
            if (!suppressEval_) {
                const auto* b = std::get_if<bool>(&*cond);
                if (!b) {
                    setError("ternary '?:' condition must be bool");
                    return std::nullopt;
                }
                condTrue = *b;
            }
            std::optional<runtime::Value> thenVal;
            {
                ScopedSuppress sup(suppressEval_, !condTrue);
                thenVal = parseConditional();
            }
            if (!thenVal || !match(":")) {
                setError("expected ':' in ternary expression");
                return std::nullopt;
            }
            std::optional<runtime::Value> elseVal;
            {
                ScopedSuppress sup(suppressEval_, condTrue);
                elseVal = parseConditional();
            }
            if (!elseVal) return std::nullopt;
            return condTrue ? thenVal : elseVal;
        }
        return cond;
    }

    std::optional<runtime::Value> parseNullCoalescing() {
        auto left = parseLogicalOr();
        if (!left) return std::nullopt;
        while (match("??")) {
            const bool leftIsNull = !suppressEval_ && (isNullValue(*left) || isNullableNull(*left));
            std::optional<runtime::Value> right;
            {
                ScopedSuppress sup(suppressEval_, !leftIsNull);
                right = parseLogicalOr();
            }
            if (!right) return std::nullopt;
            if (!suppressEval_) {
                if (leftIsNull) {
                    left = *right;
                } else if (auto unwrapped = unwrapNullableIfPresent(*left)) {
                    left = *unwrapped;
                }
            }
        }
        return left;
    }

    std::optional<runtime::Value> parseLogicalOr() {
        auto left = parseLogicalAnd();
        if (!left) return std::nullopt;
        while (match("||")) {
            bool lhsBool = false;
            if (!suppressEval_) {
                const auto* lb = std::get_if<bool>(&*left);
                if (!lb) {
                    setError("'||' requires bool operands");
                    return std::nullopt;
                }
                lhsBool = *lb;
            }
            std::optional<runtime::Value> right;
            {
                ScopedSuppress sup(suppressEval_, lhsBool);
                right = parseLogicalAnd();
            }
            if (!right) return std::nullopt;
            if (!suppressEval_) {
                if (lhsBool) {
                    left = runtime::Value{true};
                } else {
                    const auto* rb = std::get_if<bool>(&*right);
                    if (!rb) {
                        setError("'||' requires bool operands");
                        return std::nullopt;
                    }
                    left = runtime::Value{*rb};
                }
            }
        }
        return left;
    }

    std::optional<runtime::Value> parseLogicalAnd() {
        auto left = parseEquality();
        if (!left) return std::nullopt;
        while (match("&&")) {
            bool lhsBool = true;
            if (!suppressEval_) {
                const auto* lb = std::get_if<bool>(&*left);
                if (!lb) {
                    setError("'&&' requires bool operands");
                    return std::nullopt;
                }
                lhsBool = *lb;
            }
            std::optional<runtime::Value> right;
            {
                ScopedSuppress sup(suppressEval_, !lhsBool);
                right = parseEquality();
            }
            if (!right) return std::nullopt;
            if (!suppressEval_) {
                if (!lhsBool) {
                    left = runtime::Value{false};
                } else {
                    const auto* rb = std::get_if<bool>(&*right);
                    if (!rb) {
                        setError("'&&' requires bool operands");
                        return std::nullopt;
                    }
                    left = runtime::Value{*rb};
                }
            }
        }
        return left;
    }

    std::optional<runtime::Value> parseEquality() {
        auto left = parseComparison();
        if (!left) return std::nullopt;
        for (;;) {
            const bool isEq = !peek("===") && match("==");
            const bool isNe = !isEq && match("!=");
            if (!isEq && !isNe) break;
            auto right = parseComparison();
            if (!right) return std::nullopt;
            if (suppressEval_) {
                left = runtime::Value{false};
                continue;
            }
            const bool equal = valuesEqualForEval(*left, *right, heap_);
            left = runtime::Value{isEq ? equal : !equal};
        }
        return left;
    }

    std::optional<runtime::Value> parseComparison() {
        auto left = parseAdditive();
        if (!left) return std::nullopt;
        for (;;) {
            enum class Op { None, Le, Ge, Lt, Gt };
            Op op = Op::None;
            if (match("<=")) op = Op::Le;
            else if (match(">=")) op = Op::Ge;
            else if (match("<")) op = Op::Lt;
            else if (match(">")) op = Op::Gt;
            if (op == Op::None) break;

            auto right = parseAdditive();
            if (!right) return std::nullopt;
            if (suppressEval_) {
                left = runtime::Value{false};
                continue;
            }
            std::int64_t li = 0;
            std::int64_t ri = 0;
            if (tryAsInt64(*left, li) && tryAsInt64(*right, ri)) {
                switch (op) {
                case Op::Le: left = runtime::Value{li <= ri}; break;
                case Op::Ge: left = runtime::Value{li >= ri}; break;
                case Op::Lt: left = runtime::Value{li < ri}; break;
                case Op::Gt: left = runtime::Value{li > ri}; break;
                case Op::None: break;
                }
                continue;
            }
            double ld = 0.0;
            double rd = 0.0;
            if (tryAsDouble(*left, ld) && tryAsDouble(*right, rd)) {
                switch (op) {
                case Op::Le: left = runtime::Value{ld <= rd}; break;
                case Op::Ge: left = runtime::Value{ld >= rd}; break;
                case Op::Lt: left = runtime::Value{ld < rd}; break;
                case Op::Gt: left = runtime::Value{ld > rd}; break;
                case Op::None: break;
                }
                continue;
            }
            setError("comparison operator requires numeric operands");
            return std::nullopt;
        }
        return left;
    }

    std::optional<runtime::Value> parseAdditive() {
        auto left = parseMultiplicative();
        if (!left) return std::nullopt;
        for (;;) {
            const bool isAdd = match("+");
            const bool isSub = !isAdd && match("-");
            if (!isAdd && !isSub) break;
            auto right = parseMultiplicative();
            if (!right) return std::nullopt;
            if (suppressEval_) {
                left = runtime::Value{std::int64_t{0}};
                continue;
            }
            if (isAdd) {
                std::string ls;
                std::string rs;
                if (tryAsString(*left, heap_, ls) || tryAsString(*right, heap_, rs)) {
                    const std::string ltext = tryAsString(*left, heap_, ls)
                        ? ls : runtime::valueToString(*left, heap_);
                    const std::string rtext = tryAsString(*right, heap_, rs)
                        ? rs : runtime::valueToString(*right, heap_);
                    left = runtime::Value{ltext + rtext};
                    continue;
                }
            }
            std::int64_t li = 0;
            std::int64_t ri = 0;
            if (tryAsInt64(*left, li) && tryAsInt64(*right, ri)) {
                left = runtime::Value{isAdd ? (li + ri) : (li - ri)};
                continue;
            }
            double ld = 0.0;
            double rd = 0.0;
            if (tryAsDouble(*left, ld) && tryAsDouble(*right, rd)) {
                left = runtime::Value{isAdd ? (ld + rd) : (ld - rd)};
                continue;
            }
            setError("arithmetic operator requires numeric operands");
            return std::nullopt;
        }
        return left;
    }

    std::optional<runtime::Value> parseMultiplicative() {
        auto left = parseUnary();
        if (!left) return std::nullopt;
        for (;;) {
            enum class Op { None, Mul, Div, Mod };
            Op op = Op::None;
            if (match("*")) op = Op::Mul;
            else if (match("/")) op = Op::Div;
            else if (match("%")) op = Op::Mod;
            if (op == Op::None) break;

            auto right = parseUnary();
            if (!right) return std::nullopt;
            if (suppressEval_) {
                left = runtime::Value{std::int64_t{0}};
                continue;
            }
            std::int64_t li = 0;
            std::int64_t ri = 0;
            if (tryAsInt64(*left, li) && tryAsInt64(*right, ri)) {
                if ((op == Op::Div || op == Op::Mod) && ri == 0) {
                    setError("division by zero");
                    return std::nullopt;
                }
                if (op == Op::Mul) left = runtime::Value{li * ri};
                else if (op == Op::Div) left = runtime::Value{li / ri};
                else left = runtime::Value{li % ri};
                continue;
            }
            double ld = 0.0;
            double rd = 0.0;
            if (op != Op::Mod && tryAsDouble(*left, ld) && tryAsDouble(*right, rd)) {
                if (op == Op::Div && rd == 0.0) {
                    setError("division by zero");
                    return std::nullopt;
                }
                left = runtime::Value{op == Op::Mul ? (ld * rd) : (ld / rd)};
                continue;
            }
            setError("multiplicative operator requires numeric operands");
            return std::nullopt;
        }
        return left;
    }

    std::optional<runtime::Value> parseUnary() {
        if (match("!")) {
            auto operand = parseUnary();
            if (!operand) return std::nullopt;
            if (suppressEval_) return runtime::Value{false};
            if (const auto* b = std::get_if<bool>(&*operand)) {
                return runtime::Value{!*b};
            }
            setError("'!' requires bool operand");
            return std::nullopt;
        }
        if (match("-")) {
            auto operand = parseUnary();
            if (!operand) return std::nullopt;
            if (suppressEval_) return runtime::Value{std::int64_t{0}};
            std::int64_t integer = 0;
            if (tryAsInt64(*operand, integer)) return runtime::Value{-integer};
            double number = 0.0;
            if (tryAsDouble(*operand, number)) return runtime::Value{-number};
            setError("unary '-' requires numeric operand");
            return std::nullopt;
        }
        if (match("+")) {
            return parseUnary();
        }
        return parsePostfix();
    }

    bool parseArgumentList(std::vector<runtime::Value>& outArgs, bool skipValues) {
        outArgs.clear();
        if (match(")")) return true;
        for (;;) {
            std::optional<runtime::Value> argVal;
            {
                ScopedSuppress sup(suppressEval_, skipValues);
                argVal = parseConditional();
            }
            if (!argVal) return false;
            outArgs.push_back(std::move(*argVal));
            if (match(",")) continue;
            if (match(")")) return true;
            setError("expected ',' or ')' in argument list");
            return false;
        }
    }

    std::optional<runtime::Value> parsePostfix() {
        semantic::SymbolId pendingStaticTypeId = 0;
        auto current = parsePrimary(&pendingStaticTypeId);
        if (!current) return std::nullopt;
        bool nullShortCircuited = false;

        for (;;) {
            const bool nullCondDot = match("?.");
            const bool normalDot = !nullCondDot && match(".");
            if (nullCondDot || normalDot) {
                const auto member = parseIdentifier();
                if (member.empty()) {
                    setError("expected member name after '.'");
                    return std::nullopt;
                }
                const bool hasCallParens = match("(");
                std::vector<runtime::Value> callArgs;
                const bool shouldSkipArgs =
                    suppressEval_ || nullShortCircuited ||
                    (nullCondDot && pendingStaticTypeId == 0 &&
                     (isNullValue(*current) || isNullableNull(*current)));
                if (hasCallParens) {
                    if (!parseArgumentList(callArgs, shouldSkipArgs)) return std::nullopt;
                }

                if (suppressEval_) {
                    pendingStaticTypeId = 0;
                    current = runtime::Value{std::int64_t{0}};
                    continue;
                }
                if (nullShortCircuited) {
                    continue;
                }
                if (pendingStaticTypeId != 0) {
                    const auto tid = pendingStaticTypeId;
                    pendingStaticTypeId = 0;
                    current = resolveStaticMember(tid, member, hasCallParens ? &callArgs : nullptr);
                    if (!current) return std::nullopt;
                    continue;
                }
                if (nullCondDot && (isNullValue(*current) || isNullableNull(*current))) {
                    nullShortCircuited = true;
                    current = runtime::Value{runtime::NullObject{}};
                    continue;
                }
                runtime::Value effectiveTarget = *current;
                if (nullCondDot) {
                    if (auto unwrapped = unwrapNullableIfPresent(effectiveTarget)) {
                        effectiveTarget = *unwrapped;
                    }
                }
                current = resolveMemberOrCall(
                    effectiveTarget, member, hasCallParens ? &callArgs : nullptr);
                if (!current) return std::nullopt;
                continue;
            }

            const bool nullCondBracket = match("?[");
            const bool normalBracket = !nullCondBracket && match("[");
            if (nullCondBracket || normalBracket) {
                if (pendingStaticTypeId != 0) {
                    setError("cannot index a type name");
                    return std::nullopt;
                }
                const bool targetIsNull = !suppressEval_ && !nullShortCircuited &&
                    nullCondBracket && (isNullValue(*current) || isNullableNull(*current));
                std::optional<runtime::Value> indexVal;
                {
                    ScopedSuppress sup(suppressEval_, nullShortCircuited || targetIsNull);
                    indexVal = parseConditional();
                }
                if (!indexVal || !match("]")) {
                    setError("invalid index expression");
                    return std::nullopt;
                }
                if (suppressEval_) {
                    current = runtime::Value{std::int64_t{0}};
                    continue;
                }
                if (nullShortCircuited) {
                    continue;
                }
                if (targetIsNull) {
                    nullShortCircuited = true;
                    current = runtime::Value{runtime::NullObject{}};
                    continue;
                }
                current = resolveIndex(*current, *indexVal);
                if (!current) return std::nullopt;
                continue;
            }
            break;
        }
        if (pendingStaticTypeId != 0 && !suppressEval_) {
            setError("type name cannot be used as a standalone value");
            return std::nullopt;
        }
        return current;
    }

    std::string parseIdentifier() {
        skipWhitespace();
        if (pos_ >= text_.size()) return {};
        const unsigned char first = static_cast<unsigned char>(text_[pos_]);
        if (!std::isalpha(first) && first != '_' && first != '$') return {};
        const std::size_t start = pos_++;
        while (pos_ < text_.size()) {
            const unsigned char ch = static_cast<unsigned char>(text_[pos_]);
            if (!std::isalnum(ch) && ch != '_' && ch != '$') break;
            ++pos_;
        }
        return text_.substr(start, pos_ - start);
    }

    std::optional<runtime::Value> parsePrimary(semantic::SymbolId* outStaticTypeId) {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            setError("unexpected end of expression");
            return std::nullopt;
        }
        if (match("(")) {
            auto inner = parseConditional();
            if (!inner || !match(")")) {
                setError("expected ')'");
                return std::nullopt;
            }
            return inner;
        }
        if (text_[pos_] == '"') {
            ++pos_;
            std::string literal;
            while (pos_ < text_.size() && text_[pos_] != '"') {
                if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
                    ++pos_;
                    switch (text_[pos_]) {
                    case 'n': literal.push_back('\n'); break;
                    case 't': literal.push_back('\t'); break;
                    case 'r': literal.push_back('\r'); break;
                    default: literal.push_back(text_[pos_]); break;
                    }
                } else {
                    literal.push_back(text_[pos_]);
                }
                ++pos_;
            }
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                setError("unterminated string literal");
                return std::nullopt;
            }
            ++pos_;
            return runtime::Value{std::move(literal)};
        }
        if (std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            const char* startPtr = text_.c_str() + pos_;
            char* endInt = nullptr;
            const long long intVal = std::strtoll(startPtr, &endInt, 10);
            const std::size_t intLen = static_cast<std::size_t>(endInt - startPtr);
            if (pos_ + intLen < text_.size() &&
                (text_[pos_ + intLen] == '.' || text_[pos_ + intLen] == 'e' || text_[pos_ + intLen] == 'E')) {
                char* endDouble = nullptr;
                const double dblVal = std::strtod(startPtr, &endDouble);
                pos_ += static_cast<std::size_t>(endDouble - startPtr);
                if (pos_ < text_.size() && (text_[pos_] == 'f' || text_[pos_] == 'F')) ++pos_;
                return runtime::Value{dblVal};
            }
            pos_ += intLen;
            if (pos_ < text_.size() && (text_[pos_] == 'L' || text_[pos_] == 'l')) {
                ++pos_;
                return runtime::Value{runtime::LongValue{static_cast<std::int64_t>(intVal)}};
            }
            return runtime::Value{static_cast<std::int64_t>(intVal)};
        }
        auto ident = parseIdentifier();
        if (ident.empty()) {
            setError("unexpected token in expression");
            return std::nullopt;
        }
        if (ident == "true") return runtime::Value{true};
        if (ident == "false") return runtime::Value{false};
        if (ident == "null") return runtime::Value{runtime::NullObject{}};

        // Support Module.Sub::TypeName or Type::Member qualified prefix
        if (peek(".") || peek("::")) {
            const std::size_t savedPos = pos_;
            std::string fullQual = ident;
            bool sawScope = false;
            for (;;) {
                if (match("::")) {
                    const auto nextSeg = parseIdentifier();
                    if (nextSeg.empty()) { pos_ = savedPos; break; }
                    fullQual += "::" + nextSeg;
                    sawScope = true;
                    break;
                }
                // Check if ident.Next::Type occurs
                const std::size_t beforeDot = pos_;
                if (match(".")) {
                    const auto nextSeg = parseIdentifier();
                    if (!nextSeg.empty() && (peek(".") || peek("::"))) {
                        fullQual += "." + nextSeg;
                        continue;
                    }
                }
                pos_ = beforeDot;
                break;
            }
            if (sawScope) {
                if (const auto tIt = typesByName_.find(fullQual); tIt != typesByName_.end()) {
                    if (outStaticTypeId) *outStaticTypeId = tIt->second;
                    return runtime::Value{std::int64_t{0}};
                }
                if (program_) {
                    if (const auto fnId = program_->findFunction(fullQual); fnId && match("(")) {
                        std::vector<runtime::Value> args;
                        if (!parseArgumentList(args, suppressEval_)) return std::nullopt;
                        if (suppressEval_) return runtime::Value{std::int64_t{0}};
                        return invokePureSandbox(*fnId, args, fullQual);
                    }
                }
                pos_ = savedPos;
            } else {
                pos_ = savedPos;
            }
        }

        if (suppressEval_) {
            if (match("(")) {
                std::vector<runtime::Value> dummyArgs;
                if (!parseArgumentList(dummyArgs, true)) return std::nullopt;
            }
            return runtime::Value{std::int64_t{0}};
        }

        // Unqualified call `Method(args)` on `this` or current module free function
        if (match("(")) {
            std::vector<runtime::Value> callArgs;
            if (!parseArgumentList(callArgs, false)) return std::nullopt;
            for (const auto& arg : frame_.arguments) {
                if (arg.name == "this") {
                    const std::string prevErr = error_;
                    error_.clear();
                    if (auto mVal = resolveMemberOrCall(arg.value, ident, &callArgs)) {
                        return mVal;
                    }
                    if (error_.find("side effects") != std::string::npos) {
                        return std::nullopt;
                    }
                    error_ = prevErr;
                }
            }
            if (program_) {
                const auto sep = frame_.function.find("::");
                if (sep != std::string::npos) {
                    const std::string qualFn = frame_.function.substr(0, sep) + "::" + ident;
                    if (const auto fnId = program_->findFunction(qualFn)) {
                        return invokePureSandbox(*fnId, callArgs, ident);
                    }
                }
            }
            setError("unknown or impure function '" + ident + "'");
            return std::nullopt;
        }

        for (auto it = frame_.locals.rbegin(); it != frame_.locals.rend(); ++it) {
            if (it->name == ident) return it->value;
        }
        for (const auto& arg : frame_.arguments) {
            if (arg.name == ident) return arg.value;
        }
        for (const auto& arg : frame_.arguments) {
            if (arg.name == "this") {
                const std::string prevErr = error_;
                error_.clear();
                if (auto mVal = resolveMemberOrCall(arg.value, ident, nullptr)) {
                    return mVal;
                }
                if (error_.find("side effects") != std::string::npos) {
                    return std::nullopt;
                }
                error_ = prevErr;
            }
        }
        if (const auto tIt = typesByName_.find(ident); tIt != typesByName_.end()) {
            if (outStaticTypeId) *outStaticTypeId = tIt->second;
            return runtime::Value{std::int64_t{0}};
        }
        setError("unknown identifier '" + ident + "'");
        return std::nullopt;
    }

    const std::string& text_;
    const DebugStackFrame& frame_;
    const std::unordered_map<semantic::SymbolId, semantic::TypeSymbol>& types_;
    const std::unordered_map<std::string, semantic::SymbolId>& typesByName_;
    const runtime::ProgramImage* program_ = nullptr;
    runtime::ManagedHeap* heap_ = nullptr;
    std::size_t pos_ = 0;
    bool suppressEval_ = false;
    std::string error_;
};

} // namespace

void DebugController::bindProgram(const runtime::ProgramImage& program) {
    bindModules(program.modules());
    std::lock_guard<std::mutex> lock(mutex_);
    boundProgram_ = &program;
    synthesizedProgram_.reset();
}

void DebugController::bindModules(const std::vector<bytecode::Module>& modules) {
    std::shared_ptr<const runtime::ProgramImage> linkedFallback;
    if (!modules.empty()) {
        runtime::RuntimeError linkErr;
        if (auto linked = runtime::ProgramImage::link(modules, linkErr)) {
            linkedFallback = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool hadModulesBefore = !executableLocations_.empty();
    executableLocations_.clear();
    types_.clear();
    typesByName_.clear();
    boundProgram_ = nullptr;
    synthesizedProgram_ = std::move(linkedFallback);
    for (const auto& module : modules) {
        for (const auto& type : module.types) {
            types_[type.id] = type;
            typesByName_[semantic::canonicalTypeName(type)] = type.id;
            if (!type.synthetic || type.name.find("__") == std::string::npos) {
                typesByName_.emplace(type.name, type.id);
            }
        }
        for (const auto& function : module.functions) {
            for (const auto& point : function.debugInfo.sequencePoints) {
                if (point.hidden) continue;
                std::string effectivePath = sequencePointSourcePath(module, point);
                if (effectivePath.empty()) continue;
                executableLocations_[effectivePath].push_back({
                    effectivePath,
                    point.range.start.line,
                    point.range.start.column,
                });
            }
        }
    }
    for (auto& [path, locations] : executableLocations_) {
        (void)path;
        std::sort(locations.begin(), locations.end(),
            [](const BoundLocation& left, const BoundLocation& right) {
                if (left.line != right.line) return left.line < right.line;
                return left.column < right.column;
            });
        locations.erase(std::unique(locations.begin(), locations.end(),
            [](const BoundLocation& left, const BoundLocation& right) {
                return left.line == right.line && left.column == right.column;
            }), locations.end());
    }
    for (auto& breakpoint : breakpoints_) {
        breakpoint.verified = false;
        auto found = executableLocations_.find(breakpoint.sourcePath);
        if (found == executableLocations_.end()) {
            for (auto it = executableLocations_.begin(); it != executableLocations_.end(); ++it) {
                if (debugPathsMatch(breakpoint.sourcePath, it->first)) {
                    found = it;
                    clientPathAliases_[it->first] = breakpoint.sourcePath;
                    break;
                }
            }
        }
        if (found == executableLocations_.end()) continue;
        const std::size_t targetLine =
            breakpoint.requestedLine != 0 ? breakpoint.requestedLine : breakpoint.line;
        const auto location = std::lower_bound(
            found->second.begin(), found->second.end(), targetLine,
            [](const BoundLocation& value, std::size_t line) {
                return value.line < line;
            });
        if (location != found->second.end()) {
            breakpoint.line = location->line;
            breakpoint.column = location->column;
            breakpoint.verified = true;
        }
    }
    if (!hadModulesBefore) {
        entryPending_ = stopOnEntry_;
    }
}

std::vector<Breakpoint> DebugController::setBreakpoints(
    std::string sourcePath,
    const std::vector<text::LinePosition>& positions) {
    std::vector<BreakpointSpec> specs;
    specs.reserve(positions.size());
    for (const auto& position : positions) {
        specs.push_back(BreakpointSpec{position, {}});
    }
    return setConditionalBreakpoints(std::move(sourcePath), specs);
}

std::vector<Breakpoint> DebugController::setConditionalBreakpoints(
    std::string sourcePath,
    const std::vector<BreakpointSpec>& specs) {
    std::lock_guard<std::mutex> lock(mutex_);
    breakpoints_.erase(
        std::remove_if(breakpoints_.begin(), breakpoints_.end(),
            [&](const Breakpoint& breakpoint) {
                return debugPathsMatch(breakpoint.sourcePath, sourcePath);
            }),
        breakpoints_.end());

    auto executable = executableLocations_.find(sourcePath);
    if (executable == executableLocations_.end()) {
        for (auto it = executableLocations_.begin(); it != executableLocations_.end(); ++it) {
            if (debugPathsMatch(sourcePath, it->first)) {
                executable = it;
                clientPathAliases_[it->first] = sourcePath;
                break;
            }
        }
    } else {
        clientPathAliases_[sourcePath] = sourcePath;
    }

    for (const auto& spec : specs) {
        Breakpoint breakpoint;
        breakpoint.id = nextBreakpointId_++;
        breakpoint.sourcePath = sourcePath;
        breakpoint.line = spec.position.line;
        breakpoint.column = spec.position.column;
        breakpoint.requestedLine = spec.position.line;
        breakpoint.requestedColumn = spec.position.column;
        breakpoint.condition = spec.condition;
        if (executable != executableLocations_.end()) {
            const auto exact = std::lower_bound(
                executable->second.begin(), executable->second.end(), spec.position.line,
                [](const BoundLocation& location, std::size_t line) {
                    return location.line < line;
                });
            if (exact != executable->second.end()) {
                breakpoint.line = exact->line;
                breakpoint.column = exact->column;
                breakpoint.verified = true;
            }
        }
        breakpoints_.push_back(std::move(breakpoint));
    }
    return breakpoints_;
}

std::vector<Breakpoint> DebugController::breakpoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return breakpoints_;
}

void DebugController::clearBreakpoints() {
    std::lock_guard<std::mutex> lock(mutex_);
    breakpoints_.clear();
}

void DebugController::setStopHandler(StopHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    stopHandler_ = std::move(handler);
}

void DebugController::stopOnEntry(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stopOnEntry_ = enabled;
    entryPending_ = enabled;
}

void DebugController::setExceptionBreakpoints(
    bool breakOnThrown,
    bool breakOnUncaught,
    bool breakOnRuntimeError) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    breakOnThrown_ = breakOnThrown;
    breakOnUncaught_ = breakOnUncaught;
    breakOnRuntimeError_ = breakOnRuntimeError;
}

bool DebugController::breakOnThrown() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return breakOnThrown_;
}

bool DebugController::breakOnUncaught() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return breakOnUncaught_;
}

bool DebugController::breakOnRuntimeError() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return breakOnRuntimeError_;
}

void DebugController::requestPause() noexcept {
    pauseRequested_.store(true);
}

void DebugController::resetTermination() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    terminated_ = false;
    mode_ = ResumeMode::Continue;
    lastBreakpointLocation_ = {};
    lastLocation_ = {};
}

std::string DebugController::resolveSourcePath(const std::string& enginePath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = clientPathAliases_.find(enginePath);
    if (it != clientPathAliases_.end()) return it->second;
    return enginePath;
}

std::optional<runtime::Value> DebugController::evaluateInFrameUnlocked(
    const std::string& expression,
    const DebugStackFrame& frame,
    runtime::ManagedHeap* heap,
    std::string* errorMessage) const {
    const runtime::ProgramImage* activeProgram =
        boundProgram_ ? boundProgram_ : synthesizedProgram_.get();
    ExpressionEvaluator evaluator(
        expression,
        frame,
        types_,
        typesByName_,
        activeProgram,
        heap ? heap : lastHeap_);
    return evaluator.evaluate(errorMessage);
}

std::optional<runtime::Value> DebugController::evaluate(
    const std::string& expression,
    std::uint32_t frameId,
    runtime::ManagedHeap* heap,
    std::string* errorMessage) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastStop_.frames.empty()) {
        if (errorMessage) *errorMessage = "debugger has no active stack frame";
        return std::nullopt;
    }
    const DebugStackFrame* targetFrame = nullptr;
    for (const auto& frame : lastStop_.frames) {
        if (frame.id == frameId) {
            targetFrame = &frame;
            break;
        }
    }
    if (!targetFrame) targetFrame = &lastStop_.frames.front();
    return evaluateInFrameUnlocked(expression, *targetFrame, heap, errorMessage);
}

bool DebugController::setVariable(
    const std::string& name,
    const std::string& valueExpression,
    std::uint32_t frameId,
    runtime::ManagedHeap* heap,
    runtime::Value* outNewValue,
    std::string* errorMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastStop_.frames.empty() || !activeFrameViews_ || activeFrameViews_->empty()) {
        if (errorMessage) *errorMessage = "debugger has no active stack frame";
        return false;
    }
    std::size_t stopIdx = 0;
    for (std::size_t i = 0; i < lastStop_.frames.size(); ++i) {
        if (lastStop_.frames[i].id == frameId) {
            stopIdx = i;
            break;
        }
    }
    auto& targetFrame = lastStop_.frames[stopIdx];
    auto* effectiveHeap = heap ? heap : lastHeap_;
    const auto evaluated = evaluateInFrameUnlocked(
        valueExpression, targetFrame, effectiveHeap, errorMessage);
    if (!evaluated) return false;

    const std::size_t viewIdx = activeFrameViews_->size() - 1 - stopIdx;
    if (viewIdx >= activeFrameViews_->size()) return false;
    const auto& view = (*activeFrameViews_)[viewIdx];
    if (!view.function || !view.locals.values) return false;

    for (const auto& local : view.function->debugInfo.locals) {
        if (local.name != name || local.slot >= view.locals.size()) continue;
        if (!local.parameter && view.point) {
            const auto offset = view.point->range.span.start;
            if (offset < local.scope.span.start ||
                offset > local.scope.span.end()) {
                continue;
            }
        }
        runtime::Value converted = *evaluated;
        if (semantic::isNumericType(local.type)) {
            if (!runtime::tryConvertNumeric(*evaluated, local.type, converted, false)) {
                if (errorMessage) {
                    *errorMessage = "cannot convert '" +
                        std::string(semantic::primitiveTypeName(runtime::valueType(*evaluated))) +
                        "' to '" + semantic::primitiveTypeName(local.type) + "'";
                }
                return false;
            }
        } else {
            const auto actualType = runtime::valueType(converted);
            const bool isNullRef = actualType == semantic::PrimitiveType::Null &&
                semantic::isReferenceType(local.type);
            if (!isNullRef && actualType != local.type) {
                if (errorMessage) {
                    *errorMessage = "cannot assign '" +
                        std::string(semantic::primitiveTypeName(actualType)) +
                        "' to variable '" + name + "' of type '" +
                        semantic::primitiveTypeName(local.type) + "'";
                }
                return false;
            }
        }
        (*view.locals.values)[view.locals.offset + local.slot] = converted;
        if (local.parameter && view.arguments.values && local.slot < view.arguments.size()) {
            (*view.arguments.values)[view.arguments.offset + local.slot] = converted;
        }
        for (auto& v : targetFrame.locals) {
            if (v.name == name) v.value = converted;
        }
        for (auto& v : targetFrame.arguments) {
            if (v.name == name) v.value = converted;
        }
        if (outNewValue) *outNewValue = std::move(converted);
        return true;
    }
    if (errorMessage) *errorMessage = "variable '" + name + "' not found in frame";
    return false;
}

std::uint32_t DebugController::matchingBreakpoint(
    const std::vector<DebugFrameView>& frames,
    runtime::ManagedHeap* heap) const {
    if (frames.empty() || !frames.back().point || !frames.back().module) return 0;
    const auto& current = frames.back();
    if (current.point->hidden) return 0;
    const std::string effectivePath =
        sequencePointSourcePath(*current.module, *current.point);
    if (effectivePath.empty()) return 0;
    std::optional<DebugStackFrame> cachedFrame;
    for (const auto& breakpoint : breakpoints_) {
        if (!breakpoint.verified ||
            !debugPathsMatch(breakpoint.sourcePath, effectivePath) ||
            breakpoint.line != current.point->range.start.line) {
            continue;
        }
        if (!breakpoint.condition.empty()) {
            if (!cachedFrame) {
                cachedFrame = captureSingleFrame(current, 1);
            }
            const auto condVal = evaluateInFrameUnlocked(
                breakpoint.condition, *cachedFrame, heap, nullptr);
            const auto* condBool = condVal ? std::get_if<bool>(&*condVal) : nullptr;
            if (!condBool || !*condBool) continue;
        }
        return breakpoint.id;
    }
    return 0;
}

bool DebugController::shouldStop(
    const std::vector<DebugFrameView>& frames,
    runtime::ManagedHeap* heap,
    StopReason& reason,
    std::uint32_t& breakpointId) {
    if (frames.empty() || !frames.back().module) {
        return false;
    }
    if (mode_ == ResumeMode::Terminate) {
        terminated_ = true;
        return false;
    }
    if (!frames.back().point) {
        if (pauseRequested_.exchange(false)) {
            reason = StopReason::Pause;
            return true;
        }
        return false;
    }
    const auto& current = frames.back();
    const auto depth = frames.size();
    if (entryPending_) {
        entryPending_ = false;
        stopOnEntry_ = false;
        reason = StopReason::Entry;
        return true;
    }
    if (pauseRequested_.exchange(false)) {
        reason = StopReason::Pause;
        return true;
    }
    const bool sameBreakpointLine =
        lastBreakpointLocation_.fileId != 0 &&
        current.point->range.fileId == lastBreakpointLocation_.fileId &&
        current.point->range.start.line == lastBreakpointLocation_.start.line;
    if (!sameBreakpointLine) {
        lastBreakpointLocation_ = {};
        breakpointId = matchingBreakpoint(frames, heap);
        if (breakpointId != 0) {
            lastBreakpointLocation_ = current.point->range;
            reason = StopReason::Breakpoint;
            return true;
        }
    }
    const auto changed = !sameLocation(current.point->range, lastLocation_);
    switch (mode_) {
    case ResumeMode::StepIn:
        if (!current.point->hidden && changed) {
            reason = StopReason::Step;
            return true;
        }
        break;
    case ResumeMode::StepOver:
        if (!current.point->hidden && changed && depth <= stepDepth_) {
            reason = StopReason::Step;
            return true;
        }
        break;
    case ResumeMode::StepOut:
        if (depth < stepDepth_) {
            reason = StopReason::Step;
            return true;
        }
        break;
    case ResumeMode::Terminate:
        terminated_ = true;
        return false;
    case ResumeMode::Continue:
        break;
    }
    return false;
}

DebugStop DebugController::capture(
    StopReason reason,
    std::uint32_t breakpointId,
    const std::vector<DebugFrameView>& frames,
    runtime::ManagedHeap* heap) const {
    (void)heap;
    DebugStop stop;
    stop.reason = reason;
    stop.breakpointId = breakpointId;
    std::uint32_t frameId = 1;
    for (auto view = frames.rbegin(); view != frames.rend(); ++view) {
        if (!view->module || !view->function) continue;
        stop.frames.push_back(captureSingleFrame(*view, frameId++));
    }
    return stop;
}

bool DebugController::onSequencePoint(
    const std::vector<DebugFrameView>& frames,
    runtime::ManagedHeap* heap) {
    const auto tid = std::this_thread::get_id();
    if (safepointRequested_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> spLock(safepointMutex_);
        if (safepointRequested_.load(std::memory_order_relaxed) || safepointActive_) {
            DebugStop priorStop;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                lastHeap_ = heap;
                priorStop = lastStop_;
                lastStop_ = capture(StopReason::Pause, 0, frames, heap);
                activeFrameViews_ = &frames;
            }
            bool incrementedStopped = false;
            auto it = activeThreads_.find(tid);
            if (it != activeThreads_.end()) {
                if (!it->second.stopped) {
                    it->second.stopped = true;
                    ++stoppedThreads_;
                    incrementedStopped = true;
                }
            } else {
                ++stoppedThreads_;
                incrementedStopped = true;
            }
            safepointCv_.notify_all();
            safepointCv_.wait(spLock, [this] {
                return !safepointRequested_.load(std::memory_order_relaxed) && !safepointActive_;
            });
            if (incrementedStopped) {
                auto itAfter = activeThreads_.find(tid);
                if (itAfter != activeThreads_.end()) {
                    itAfter->second.stopped = false;
                }
                if (stoppedThreads_ > 0) --stoppedThreads_;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                activeFrameViews_ = nullptr;
                lastStop_ = std::move(priorStop);
            }
            safepointCv_.notify_all();
        }
    }

    StopHandler handler;
    StopReason reason = StopReason::Breakpoint;
    std::uint32_t breakpointId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastHeap_ = heap;
        if (terminated_) return false;
        if (!shouldStop(frames, heap, reason, breakpointId)) return true;
        lastStop_ = capture(reason, breakpointId, frames, heap);
        activeFrameViews_ = &frames;
        handler = stopHandler_;
    }

    bool incrementedStopped = false;
    {
        std::lock_guard<std::mutex> spLock(safepointMutex_);
        auto it = activeThreads_.find(tid);
        if (it != activeThreads_.end()) {
            if (!it->second.stopped) {
                it->second.stopped = true;
                ++stoppedThreads_;
                incrementedStopped = true;
            }
        } else {
            ++stoppedThreads_;
            incrementedStopped = true;
        }
        safepointCv_.notify_all();
    }
    const auto next = handler ? handler(lastStop_) : ResumeMode::Continue;
    {
        std::unique_lock<std::mutex> spLock(safepointMutex_);
        safepointCv_.wait(spLock, [this] {
            return !safepointRequested_.load(std::memory_order_relaxed) && !safepointActive_;
        });
        if (incrementedStopped) {
            auto itAfter = activeThreads_.find(tid);
            if (itAfter != activeThreads_.end()) {
                itAfter->second.stopped = false;
            }
            if (stoppedThreads_ > 0) --stoppedThreads_;
        }
        safepointCv_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeFrameViews_ = nullptr;
        mode_ = next;
        stepDepth_ = frames.size();
        if (!frames.empty() && frames.back().point) {
            lastLocation_ = frames.back().point->range;
        }
        if (next == ResumeMode::Terminate) {
            terminated_ = true;
            return false;
        }
    }
    return true;
}

bool DebugController::onException(
    const std::vector<DebugFrameView>& frames,
    runtime::ManagedHeap* heap,
    runtime::ErrorCode code,
    std::string exceptionId,
    std::string description,
    bool isUncaught,
    bool isRuntimeError) {
    if (frames.empty() || !frames.back().module) return true;
    StopHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastHeap_ = heap;
        if (terminated_) return false;
        const bool shouldBreak =
            breakOnThrown_ ||
            (isUncaught && breakOnUncaught_) ||
            (isRuntimeError && breakOnRuntimeError_);
        if (!shouldBreak) return true;
        lastStop_ = capture(StopReason::Exception, 0, frames, heap);
        lastStop_.errorCode = code;
        lastStop_.exceptionId = std::move(exceptionId);
        lastStop_.exceptionDescription = std::move(description);
        lastStop_.exceptionUncaught = isUncaught;
        activeFrameViews_ = &frames;
        handler = stopHandler_;
    }

    const auto tid = std::this_thread::get_id();
    bool incrementedStopped = false;
    {
        std::lock_guard<std::mutex> spLock(safepointMutex_);
        auto it = activeThreads_.find(tid);
        if (it != activeThreads_.end()) {
            if (!it->second.stopped) {
                it->second.stopped = true;
                ++stoppedThreads_;
                incrementedStopped = true;
            }
        } else {
            ++stoppedThreads_;
            incrementedStopped = true;
        }
        safepointCv_.notify_all();
    }
    const auto next = handler ? handler(lastStop_) : ResumeMode::Continue;
    {
        std::unique_lock<std::mutex> spLock(safepointMutex_);
        safepointCv_.wait(spLock, [this] {
            return !safepointRequested_.load(std::memory_order_relaxed) && !safepointActive_;
        });
        if (incrementedStopped) {
            auto itAfter = activeThreads_.find(tid);
            if (itAfter != activeThreads_.end()) {
                itAfter->second.stopped = false;
            }
            if (stoppedThreads_ > 0) --stoppedThreads_;
        }
        safepointCv_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeFrameViews_ = nullptr;
        mode_ = next;
        stepDepth_ = frames.size();
        if (next == ResumeMode::Terminate) {
            terminated_ = true;
            return false;
        }
    }
    return true;
}

DebugStop DebugController::lastStop() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastStop_;
}
bool DebugController::terminated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminated_;
}

void DebugController::beginExecution() {
    const auto tid = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(safepointMutex_);
    auto it = activeThreads_.find(tid);
    if (it != activeThreads_.end() && it->second.depth > 0) {
        ++it->second.depth;
        return;
    }
    safepointCv_.wait(lock, [this] {
        return !safepointActive_ && !safepointRequested_.load(std::memory_order_relaxed);
    });
    activeThreads_[tid] = ActiveThreadState{1, false};
    ++activeExecutions_;
}

void DebugController::endExecution() noexcept {
    const auto tid = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(safepointMutex_);
    auto it = activeThreads_.find(tid);
    if (it != activeThreads_.end()) {
        if (it->second.depth > 1) {
            --it->second.depth;
            return;
        }
        if (it->second.stopped && stoppedThreads_ > 0) {
            --stoppedThreads_;
        }
        activeThreads_.erase(it);
    }
    if (activeExecutions_ > 0) --activeExecutions_;
    safepointCv_.notify_all();
}

bool DebugController::hasPendingInterruption() const noexcept {
    return pauseRequested_.load(std::memory_order_relaxed) ||
        safepointRequested_.load(std::memory_order_relaxed);
}

void DebugController::runAtSafepoint(const std::function<void()>& callback) const {
    if (!callback) return;
    const auto tid = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(safepointMutex_);
    if (safepointActive_ && safepointOwner_ == tid) {
        lock.unlock();
        callback();
        return;
    }
    safepointCv_.wait(lock, [this] {
        return !safepointActive_ && !safepointRequested_.load(std::memory_order_relaxed);
    });
    const auto selfIt = activeThreads_.find(tid);
    const std::size_t selfUnstopped =
        (selfIt != activeThreads_.end() && selfIt->second.depth > 0 && !selfIt->second.stopped)
            ? 1u
            : 0u;
    safepointRequested_.store(true, std::memory_order_release);
    safepointCv_.wait(lock, [this, selfUnstopped] {
        const std::size_t required =
            activeExecutions_ > selfUnstopped ? (activeExecutions_ - selfUnstopped) : 0u;
        return stoppedThreads_ >= required;
    });
    safepointActive_ = true;
    safepointOwner_ = tid;
    safepointRequested_.store(false, std::memory_order_release);
    lock.unlock();

    try {
        callback();
    } catch (...) {
        lock.lock();
        safepointActive_ = false;
        safepointOwner_ = std::thread::id{};
        safepointCv_.notify_all();
        throw;
    }

    lock.lock();
    safepointActive_ = false;
    safepointOwner_ = std::thread::id{};
    safepointCv_.notify_all();
}

const char* stopReasonName(StopReason reason) noexcept {
    switch (reason) {
    case StopReason::Entry: return "entry";
    case StopReason::Breakpoint: return "breakpoint";
    case StopReason::Step: return "step";
    case StopReason::Pause: return "pause";
    case StopReason::Exception: return "exception";
    }
    return "unknown";
}

const char* resumeModeName(ResumeMode mode) noexcept {
    switch (mode) {
    case ResumeMode::Continue: return "continue";
    case ResumeMode::StepIn: return "step-in";
    case ResumeMode::StepOver: return "step-over";
    case ResumeMode::StepOut: return "step-out";
    case ResumeMode::Terminate: return "terminate";
    }
    return "unknown";
}

} // namespace realscript::debug

namespace realscript::debug {

DebugSession::DebugSession(
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<runtime::ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      controller_(std::make_shared<DebugController>()) {
    if (program_) controller_->bindProgram(*program_);
    controller_->setStopHandler(
        [this](const DebugStop& stop) { return handleStop(stop); });
}

DebugSession::~DebugSession() {
    terminate();
    join();
}

std::shared_ptr<DebugController> DebugSession::controller() const noexcept {
    return controller_;
}

bool DebugSession::launch(
    std::string qualifiedFunction,
    std::vector<runtime::Value> arguments,
    bool stopOnEntry,
    runtime::Limits limits) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (launched_ || !program_) return false;
    launched_ = true;
    finished_ = false;
    paused_ = false;
    result_.reset();
    currentStop_.reset();
    terminateRequested_ = false;
    controller_->stopOnEntry(stopOnEntry);
    worker_ = std::thread([
        this,
        function = std::move(qualifiedFunction),
        values = std::move(arguments),
        limits]() mutable {
        runtime::Interpreter interpreter(program_, heap_);
        runtime::ExecutionOptions options;
        options.limits = limits;
        options.debugger = controller_;
        auto execution = interpreter.invoke(function, values, std::move(options));
        {
            std::lock_guard<std::mutex> sessionLock(sessionMutex_);
            result_ = std::move(execution);
            finished_ = true;
            paused_ = false;
            currentStop_.reset();
        }
        sessionCv_.notify_all();
    });
    return true;
}

bool DebugSession::waitForStop(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(sessionMutex_);
    const auto ready = [&] { return paused_ || finished_; };
    if (timeout.count() <= 0) {
        sessionCv_.wait(lock, ready);
        return paused_;
    }
    return sessionCv_.wait_for(lock, timeout, ready) && paused_;
}

std::optional<DebugStop> DebugSession::currentStop() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (!paused_) return std::nullopt;
    return currentStop_;
}

std::optional<runtime::Value> DebugSession::evaluate(
    const std::string& expression,
    std::uint32_t frameId,
    std::string* errorMessage) const {
    return controller_->evaluate(expression, frameId, heap_.get(), errorMessage);
}

bool DebugSession::setVariable(
    const std::string& name,
    const std::string& valueExpression,
    std::uint32_t frameId,
    runtime::Value* outNewValue,
    std::string* errorMessage) {
    const bool ok = controller_->setVariable(
        name, valueExpression, frameId, heap_.get(), outNewValue, errorMessage);
    if (ok) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        currentStop_ = controller_->lastStop();
    }
    return ok;
}

bool DebugSession::paused() const noexcept {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return paused_;
}

bool DebugSession::finished() const noexcept {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return finished_;
}

std::optional<runtime::ExecutionResult> DebugSession::result() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return result_;
}

void DebugSession::resume(ResumeMode mode) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (!paused_) return;
        pendingMode_ = mode;
        resumePending_ = true;
        paused_ = false;
        currentStop_.reset();
    }
    sessionCv_.notify_all();
}

void DebugSession::continueExecution() { resume(ResumeMode::Continue); }
void DebugSession::stepIn() { resume(ResumeMode::StepIn); }
void DebugSession::stepOver() { resume(ResumeMode::StepOver); }
void DebugSession::stepOut() { resume(ResumeMode::StepOut); }
void DebugSession::pause() { controller_->requestPause(); }

bool DebugSession::attach(bool stopOnEntry) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (worker_.joinable()) return false;
    launched_ = true;
    attached_ = true;
    finished_ = false;
    resumePending_ = false;
    terminateRequested_ = false;
    result_.reset();
    if (!paused_) {
        currentStop_.reset();
    }
    controller_->resetTermination();
    controller_->stopOnEntry(stopOnEntry);
    return true;
}

void DebugSession::detach() {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (!attached_) {
            return;
        }
        controller_->clearBreakpoints();
        controller_->stopOnEntry(false);
        pendingMode_ = ResumeMode::Continue;
        resumePending_ = true;
        terminateRequested_ = false;
        paused_ = false;
        currentStop_.reset();
        finished_ = true;
        attached_ = false;
        launched_ = false;
    }
    sessionCv_.notify_all();
}

void DebugSession::rebindProgram(std::shared_ptr<const runtime::ProgramImage> program) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto previousProgram = std::move(program_);
    program_ = std::move(program);
    if (controller_) {
        if (program_) {
            controller_->bindProgram(*program_);
        } else {
            controller_->bindModules({});
        }
    }
}

std::shared_ptr<const runtime::ProgramImage> DebugSession::program() const noexcept {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return program_;
}

bool DebugSession::isAttached() const noexcept {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return attached_;
}

void DebugSession::terminate() {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (!launched_ || finished_) return;
        if (attached_) {
            controller_->clearBreakpoints();
            controller_->stopOnEntry(false);
            pendingMode_ = ResumeMode::Continue;
            resumePending_ = true;
            terminateRequested_ = false;
            paused_ = false;
            currentStop_.reset();
            finished_ = true;
            attached_ = false;
            launched_ = false;
            sessionCv_.notify_all();
            return;
        }
        pendingMode_ = ResumeMode::Terminate;
        terminateRequested_ = true;
        resumePending_ = true;
        paused_ = false;
        currentStop_.reset();
    }
    controller_->requestPause();
    sessionCv_.notify_all();
}

void DebugSession::join() {
    if (worker_.joinable()) worker_.join();
}

void DebugSession::runAtSafepoint(const std::function<void()>& callback) const {
    if (!callback) return;
    if (controller_) {
        controller_->runAtSafepoint(callback);
    } else {
        callback();
    }
}

ResumeMode DebugSession::handleStop(const DebugStop& stop) {
    std::unique_lock<std::mutex> lock(sessionMutex_);
    if (terminateRequested_) return ResumeMode::Terminate;
    currentStop_ = stop;
    paused_ = true;
    resumePending_ = false;
    sessionCv_.notify_all();
    sessionCv_.wait(lock, [&] { return resumePending_ || terminateRequested_; });
    const auto mode = terminateRequested_ ? ResumeMode::Terminate : pendingMode_;
    resumePending_ = false;
    return mode;
}

} // namespace realscript::debug
