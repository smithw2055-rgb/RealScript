#include "realscript/debug/Debugger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
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
        if (const auto* source = sourceFile(*view.module, view.point->range.fileId)) {
            frame.sourcePath = source->path;
        }
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
    if (const auto* v = std::get_if<runtime::EnumValue>(&value)) { out = v->value; return true; }
    return false;
}

bool tryAsDouble(const runtime::Value& value, double& out) noexcept {
    if (const auto* v = std::get_if<double>(&value)) { out = *v; return true; }
    if (const auto* v = std::get_if<runtime::FloatValue>(&value)) { out = v->value; return true; }
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
    if (const auto* handle = std::get_if<runtime::NativeHandle>(&value)) {
        return !handle->valid();
    }
    return false;
}

class ExpressionEvaluator {
public:
    ExpressionEvaluator(
        const std::string& text,
        const DebugStackFrame& frame,
        const std::unordered_map<semantic::SymbolId, semantic::TypeSymbol>& types,
        runtime::ManagedHeap* heap)
        : text_(text), frame_(frame), types_(types), heap_(heap) {}

    std::optional<runtime::Value> evaluate(std::string* errorOut) {
        auto result = parseLogicalOr();
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
    void setError(std::string message) {
        if (error_.empty()) error_ = std::move(message);
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

    std::optional<runtime::Value> resolveMember(
        const runtime::Value& target,
        const std::string& member) {
        const auto structTypeId = runtime::structValueTypeId(target);
        if (structTypeId && *structTypeId != 0) {
            const auto foundType = types_.find(*structTypeId);
            if (foundType == types_.end()) {
                setError("unknown struct type metadata");
                return std::nullopt;
            }
            if (const auto* structVal = std::get_if<runtime::StructValue>(&target)) {
                if (!structVal->valid()) {
                    setError("struct storage is null");
                    return std::nullopt;
                }
                for (std::size_t index = 0; index < foundType->second.fields.size(); ++index) {
                    if (foundType->second.fields[index].name == member) {
                        if (auto fieldVal = runtime::structValueFieldAt(*structVal, index)) {
                            return *fieldVal;
                        }
                    }
                }
            } else {
                for (std::size_t index = 0; index < foundType->second.fields.size(); ++index) {
                    if (foundType->second.fields[index].name == member) {
                        if (auto fieldVal = runtime::loadInlineStructField(target, index)) {
                            return *fieldVal;
                        }
                    }
                }
            }
            setError("unknown struct field '" + member + "'");
            return std::nullopt;
        }
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&target)) {
            if (!ref->valid()) {
                setError("null object reference");
                return std::nullopt;
            }
            if (!heap_) {
                setError("managed heap is unavailable for object inspection");
                return std::nullopt;
            }
            if (ref->kind == runtime::ObjectKind::Array && member == "Length") {
                if (const auto len = heap_->arrayLength(*ref)) {
                    return static_cast<std::int64_t>(*len);
                }
            }
            if (ref->kind == runtime::ObjectKind::String && member == "Length") {
                if (const auto view = heap_->stringView(*ref)) {
                    return static_cast<std::int64_t>(view->size());
                }
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
            for (std::size_t index = 0; index < foundType->second.fields.size(); ++index) {
                if (foundType->second.fields[index].name == member) {
                    if (const auto fieldVal = heap_->fieldGet(*ref, index)) {
                        return *fieldVal;
                    }
                    break;
                }
            }
            setError("unknown object field '" + member + "'");
            return std::nullopt;
        }
        std::string strValue;
        if (tryAsString(target, heap_, strValue) && member == "Length") {
            return static_cast<std::int64_t>(strValue.size());
        }
        setError("cannot access member '" + member + "' on value");
        return std::nullopt;
    }

    std::optional<runtime::Value> parseLogicalOr() {
        auto left = parseLogicalAnd();
        if (!left) return std::nullopt;
        while (match("||")) {
            auto right = parseLogicalAnd();
            if (!right) return std::nullopt;
            const auto* lb = std::get_if<bool>(&*left);
            const auto* rb = std::get_if<bool>(&*right);
            if (!lb || !rb) {
                setError("'||' requires bool operands");
                return std::nullopt;
            }
            left = runtime::Value{*lb || *rb};
        }
        return left;
    }

    std::optional<runtime::Value> parseLogicalAnd() {
        auto left = parseEquality();
        if (!left) return std::nullopt;
        while (match("&&")) {
            auto right = parseEquality();
            if (!right) return std::nullopt;
            const auto* lb = std::get_if<bool>(&*left);
            const auto* rb = std::get_if<bool>(&*right);
            if (!lb || !rb) {
                setError("'&&' requires bool operands");
                return std::nullopt;
            }
            left = runtime::Value{*lb && *rb};
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
            bool equal = false;
            std::int64_t li = 0;
            std::int64_t ri = 0;
            double ld = 0.0;
            double rd = 0.0;
            std::string ls;
            std::string rs;
            if (isNullValue(*left) || isNullValue(*right)) {
                equal = isNullValue(*left) && isNullValue(*right);
            } else if (tryAsInt64(*left, li) && tryAsInt64(*right, ri)) {
                equal = (li == ri);
            } else if (tryAsDouble(*left, ld) && tryAsDouble(*right, rd)) {
                equal = (ld == rd);
            } else if (tryAsString(*left, heap_, ls) && tryAsString(*right, heap_, rs)) {
                equal = (ls == rs);
            } else {
                equal = (*left == *right);
            }
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
            if (const auto* b = std::get_if<bool>(&*operand)) {
                return runtime::Value{!*b};
            }
            setError("'!' requires bool operand");
            return std::nullopt;
        }
        if (match("-")) {
            auto operand = parseUnary();
            if (!operand) return std::nullopt;
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

    std::optional<runtime::Value> parsePostfix() {
        auto current = parsePrimary();
        if (!current) return std::nullopt;
        for (;;) {
            if (match(".")) {
                const auto member = parseIdentifier();
                if (member.empty()) {
                    setError("expected member name after '.'");
                    return std::nullopt;
                }
                current = resolveMember(*current, member);
                if (!current) return std::nullopt;
                continue;
            }
            if (match("[")) {
                auto indexVal = parseLogicalOr();
                if (!indexVal || !match("]")) {
                    setError("invalid index expression");
                    return std::nullopt;
                }
                std::int64_t idx = 0;
                if (!tryAsInt64(*indexVal, idx) || idx < 0) {
                    setError("array index must be a non-negative integer");
                    return std::nullopt;
                }
                const auto* ref = std::get_if<runtime::ObjectRef>(&*current);
                if (!ref || !ref->valid() || ref->kind != runtime::ObjectKind::Array || !heap_) {
                    setError("indexed value is not a valid array");
                    return std::nullopt;
                }
                const auto elem = heap_->arrayGet(*ref, static_cast<std::size_t>(idx));
                if (!elem) {
                    setError("array index out of range");
                    return std::nullopt;
                }
                current = *elem;
                continue;
            }
            break;
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

    std::optional<runtime::Value> parsePrimary() {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            setError("unexpected end of expression");
            return std::nullopt;
        }
        if (match("(")) {
            auto inner = parseLogicalOr();
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
        const auto ident = parseIdentifier();
        if (ident.empty()) {
            setError("unexpected token in expression");
            return std::nullopt;
        }
        if (ident == "true") return runtime::Value{true};
        if (ident == "false") return runtime::Value{false};
        if (ident == "null") return runtime::Value{runtime::NullObject{}};

        for (auto it = frame_.locals.rbegin(); it != frame_.locals.rend(); ++it) {
            if (it->name == ident) return it->value;
        }
        for (const auto& arg : frame_.arguments) {
            if (arg.name == ident) return arg.value;
        }
        for (const auto& arg : frame_.arguments) {
            if (arg.name == "this") {
                return resolveMember(arg.value, ident);
            }
        }
        setError("unknown identifier '" + ident + "'");
        return std::nullopt;
    }

    const std::string& text_;
    const DebugStackFrame& frame_;
    const std::unordered_map<semantic::SymbolId, semantic::TypeSymbol>& types_;
    runtime::ManagedHeap* heap_ = nullptr;
    std::size_t pos_ = 0;
    std::string error_;
};

} // namespace

void DebugController::bindProgram(const runtime::ProgramImage& program) {
    bindModules(program.modules());
}

void DebugController::bindModules(const std::vector<bytecode::Module>& modules) {
    std::lock_guard<std::mutex> lock(mutex_);
    executableLocations_.clear();
    types_.clear();
    for (const auto& module : modules) {
        for (const auto& type : module.types) {
            types_[type.id] = type;
        }
        for (const auto& function : module.functions) {
            for (const auto& point : function.debugInfo.sequencePoints) {
                const auto* source = sourceFile(module, point.range.fileId);
                if (!source) continue;
                executableLocations_[source->path].push_back({
                    source->path,
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
        const auto location = std::lower_bound(
            found->second.begin(), found->second.end(), breakpoint.line,
            [](const BoundLocation& value, std::size_t line) {
                return value.line < line;
            });
        if (location != found->second.end()) {
            breakpoint.line = location->line;
            breakpoint.column = location->column;
            breakpoint.verified = true;
        }
    }
    entryPending_ = stopOnEntry_;
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
    ExpressionEvaluator evaluator(
        expression,
        frame,
        types_,
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
    const auto* source = sourceFile(*current.module, current.point->range.fileId);
    if (!source) return 0;
    std::optional<DebugStackFrame> cachedFrame;
    for (const auto& breakpoint : breakpoints_) {
        if (!breakpoint.verified ||
            !debugPathsMatch(breakpoint.sourcePath, source->path) ||
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
        if (changed) {
            reason = StopReason::Step;
            return true;
        }
        break;
    case ResumeMode::StepOver:
        if (changed && depth <= stepDepth_) {
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
    program_ = std::move(program);
    if (program_ && controller_) {
        controller_->bindProgram(*program_);
    }
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
