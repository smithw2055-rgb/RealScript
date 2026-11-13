#include "realscript/debug/DapServer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <deque>
#include <iomanip>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace realscript::debug {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

void ensureWinsockInitialized() noexcept {
    static struct WinsockInit {
        WinsockInit() noexcept {
            WSADATA wsaData{};
            (void)WSAStartup(MAKEWORD(2, 2), &wsaData);
        }
    } s_init;
}

void closeSocketHandle(SocketHandle sock) noexcept {
    if (sock != kInvalidSocket) {
        (void)shutdown(sock, SD_BOTH);
        (void)closesocket(sock);
    }
}

int socketRecv(SocketHandle sock, char* buf, int len) noexcept {
    return ::recv(sock, buf, len, 0);
}

int socketSend(SocketHandle sock, const char* buf, int len) noexcept {
    return ::send(sock, buf, len, 0);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void ensureWinsockInitialized() noexcept {}

void closeSocketHandle(SocketHandle sock) noexcept {
    if (sock != kInvalidSocket) {
        (void)::shutdown(sock, SHUT_RDWR);
        (void)::close(sock);
    }
}

int socketRecv(SocketHandle sock, char* buf, int len) noexcept {
    return static_cast<int>(::recv(sock, buf, static_cast<std::size_t>(len), 0));
}

int socketSend(SocketHandle sock, const char* buf, int len) noexcept {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return static_cast<int>(::send(sock, buf, static_cast<std::size_t>(len), flags));
}
#endif

class SocketStreamBuf final : public std::streambuf {
public:
    explicit SocketStreamBuf(SocketHandle sock) noexcept : sock_(sock) {
        setg(inBuf_, inBuf_, inBuf_);
        setp(outBuf_, outBuf_ + sizeof(outBuf_));
    }

    ~SocketStreamBuf() override {
        (void)sync();
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (sock_ == kInvalidSocket) return traits_type::eof();
        const int n = socketRecv(sock_, inBuf_, static_cast<int>(sizeof(inBuf_)));
        if (n <= 0) {
            return traits_type::eof();
        }
        setg(inBuf_, inBuf_, inBuf_ + n);
        return traits_type::to_int_type(*gptr());
    }

    int_type overflow(int_type ch) override {
        if (flushOutput() < 0) {
            return traits_type::eof();
        }
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }

    int sync() override {
        return flushOutput();
    }

private:
    int flushOutput() noexcept {
        if (sock_ == kInvalidSocket) return -1;
        const char* p = pbase();
        int remaining = static_cast<int>(pptr() - pbase());
        while (remaining > 0) {
            const int sent = socketSend(sock_, p, remaining);
            if (sent <= 0) return -1;
            p += sent;
            remaining -= sent;
        }
        setp(outBuf_, outBuf_ + sizeof(outBuf_));
        return 0;
    }

    SocketHandle sock_ = kInvalidSocket;
    char inBuf_[4096]{};
    char outBuf_[4096]{};
};

SocketHandle createListeningTcpSocket(
    std::uint16_t port,
    const std::string& bindAddress,
    std::uint16_t* outBoundPort) noexcept {
    ensureWinsockInitialized();
    SocketHandle listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == kInvalidSocket) return kInvalidSocket;

    int opt = 1;
#if defined(_WIN32)
    (void)::setsockopt(
        listenSock, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    (void)::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (bindAddress.empty() || bindAddress == "0.0.0.0" || bindAddress == "*") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocketHandle(listenSock);
        return kInvalidSocket;
    }
    if (::listen(listenSock, 4) != 0) {
        closeSocketHandle(listenSock);
        return kInvalidSocket;
    }

    if (outBoundPort) {
        sockaddr_in bound{};
#if defined(_WIN32)
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listenSock, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
            *outBoundPort = ntohs(bound.sin_port);
        } else {
            *outBoundPort = port;
        }
    }
    return listenSock;
}

const tooling::Json* nested(
    const tooling::Json& value,
    const std::initializer_list<const char*>& keys) {
    const tooling::Json* current = &value;
    for (const auto* key : keys) current = current ? current->find(key) : nullptr;
    return current;
}

ResumeMode commandMode(const std::string& command) {
    if (command == "next") return ResumeMode::StepOver;
    if (command == "stepIn") return ResumeMode::StepIn;
    if (command == "stepOut") return ResumeMode::StepOut;
    return ResumeMode::Continue;
}

std::string formatFiberRegisterValue(
    std::int32_t rawVal,
    semantic::PrimitiveType ptype,
    semantic::SymbolId typeId,
    std::string& outTypeName) {
    static const semantic::SymbolId kFixedId =
        semantic::stableTypeId("RealScript.Math::Fixed");
    static const semantic::SymbolId kFixed16Id =
        semantic::stableTypeId("RealScript.Math::Fixed16");

    if (ptype == semantic::PrimitiveType::Struct && typeId == kFixedId) {
        outTypeName = "Fixed";
        std::ostringstream os;
        os << std::fixed << std::setprecision(4)
           << runtime::Fixed24_8::fromRaw(rawVal).toDouble()
           << " (raw=" << rawVal << ")";
        return os.str();
    }
    if (ptype == semantic::PrimitiveType::Struct && typeId == kFixed16Id) {
        outTypeName = "Fixed16";
        std::ostringstream os;
        os << std::fixed << std::setprecision(4)
           << runtime::Fixed16_16::fromRaw(rawVal).toDouble()
           << " (raw=" << rawVal << ")";
        return os.str();
    }
    if (ptype == semantic::PrimitiveType::Bool) {
        outTypeName = "bool";
        return rawVal != 0 ? "true" : "false";
    }
    if (ptype != semantic::PrimitiveType::Error &&
        ptype != semantic::PrimitiveType::Struct) {
        outTypeName = semantic::primitiveTypeName(ptype);
    } else {
        outTypeName = "int";
    }
    return std::to_string(rawVal);
}

std::uint32_t computeFiberRemainingTicks(
    const runtime::FiberScheduler& scheduler,
    const runtime::FiberContext& fiber) noexcept {
    if (fiber.status != runtime::FiberStatus::SleepingTicks) {
        return 0u;
    }
    const std::uint32_t cur = scheduler.currentTick();
    return fiber.wakeTick > cur ? (fiber.wakeTick - cur) : 0u;
}

std::uint32_t resolveFiberFramePc(
    const runtime::CompiledFiberModule* fiberModule,
    const runtime::FiberContext& fiber,
    std::size_t depthIdx) noexcept {
    if (depthIdx == 0) {
        return fiberModule
            ? fiberModule->effectiveInspectionPc(fiber)
            : fiber.pc;
    }
    if (depthIdx <= fiber.callDepth && fiber.callDepth <= fiber.callStack.size()) {
        const std::uint32_t retPc = fiber.callStack[fiber.callDepth - depthIdx];
        return retPc > 0u ? (retPc - 1u) : 0u;
    }
    return fiber.pc;
}

} // namespace

DapServer::DapServer(
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<runtime::ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      session_(std::make_shared<DebugSession>(program_, heap_)) {}

DapServer::DapServer(
    std::shared_ptr<DebugSession> session,
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<runtime::ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      session_(session ? std::move(session) : std::make_shared<DebugSession>(program_, heap_)) {}

void DapServer::rebindProgram(std::shared_ptr<const runtime::ProgramImage> program) {
    program_ = std::move(program);
    if (session_) {
        session_->rebindProgram(program_);
    }
}

void DapServer::bindFiberScheduler(
    const runtime::FiberScheduler* scheduler,
    const runtime::CompiledFiberModule* fiberModule) noexcept {
    fiberScheduler_ = scheduler;
    fiberModule_ = fiberModule;
}

void DapServer::bindFrameTelemetry(
    const runtime::FrameTelemetryCollector* telemetry) noexcept {
    frameTelemetry_ = telemetry;
}

tooling::Json DapServer::serializeFrameTelemetrySample(
    const runtime::FrameTelemetrySample& sample) const {
    tooling::Json::Array hotArr;
    for (std::uint8_t i = 0; i < sample.hotFunctionCount; ++i) {
        const auto& hf = sample.topFunctions[i];
        const auto* fnLoc = program_ ? program_->resolveFunction(hf.symbolId) : nullptr;
        hotArr.emplace_back(tooling::Json::Object{
            {"symbolId", static_cast<std::int64_t>(hf.symbolId)},
            {"name", fnLoc ? fnLoc->qualifiedName : std::string{}},
            {"instructions", static_cast<std::int64_t>(hf.instructions)},
            {"share", hf.instructionShare},
        });
    }
    return tooling::Json::Object{
        {"frameNumber", static_cast<std::int64_t>(sample.frameNumber)},
        {"totalInstructions", static_cast<std::int64_t>(sample.totalInstructions)},
        {"fiberInstructions", static_cast<std::int64_t>(sample.fiberInstructions)},
        {"compactHeapLiveBytes", static_cast<std::int64_t>(sample.compactHeapLiveBytes)},
        {"boxedHeapLiveBytes", static_cast<std::int64_t>(sample.boxedHeapLiveBytes)},
        {"gcSliceNanoseconds", static_cast<std::int64_t>(sample.gcSliceNanoseconds)},
        {"activeCoroutines", static_cast<std::int64_t>(sample.activeCoroutines)},
        {"activeFibers", static_cast<std::int64_t>(sample.activeFibers)},
        {"topFunctions", tooling::Json(std::move(hotArr))},
    };
}

tooling::Json DapServer::buildFrameTelemetryReport(std::size_t maxFrames) const {
    const runtime::FrameTelemetryCollector* collector =
        frameTelemetry_ ? frameTelemetry_ : runtime::activeFrameTelemetry();
    if (!collector) {
        return tooling::Json::Object{
            {"capacity", 0},
            {"totalFramesPublished", 0},
            {"returnedFrames", 0},
            {"latestFrame", nullptr},
            {"frames", tooling::Json::Array{}},
            {"summaryText", "No FrameTelemetryCollector bound to DapServer."},
        };
    }

    std::array<runtime::FrameTelemetrySample, runtime::FrameTelemetryCollector::kRingCapacity> samples{};
    const std::size_t clampedMax = std::min<std::size_t>(
        maxFrames == 0 ? 16u : maxFrames,
        runtime::FrameTelemetryCollector::kRingCapacity);
    const std::size_t count = collector->copyRecentSamples(samples.data(), clampedMax);

    tooling::Json::Array framesJson;
    framesJson.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        framesJson.push_back(serializeFrameTelemetrySample(samples[i]));
    }

    tooling::Json latestJson = (count > 0)
        ? serializeFrameTelemetrySample(samples[count - 1u])
        : tooling::Json(nullptr);

    return tooling::Json::Object{
        {"capacity", static_cast<std::int64_t>(runtime::FrameTelemetryCollector::capacity())},
        {"totalFramesPublished", static_cast<std::int64_t>(collector->totalFramesPublished())},
        {"returnedFrames", static_cast<std::int64_t>(count)},
        {"latestFrame", std::move(latestJson)},
        {"frames", tooling::Json(std::move(framesJson))},
        {"summaryText", collector->formatSummary(program_.get(), clampedMax)},
    };
}

void DapServer::emitFrameTelemetry(std::ostream& output) {
    const runtime::FrameTelemetryCollector* collector =
        frameTelemetry_ ? frameTelemetry_ : runtime::activeFrameTelemetry();
    if (!collector) return;

    runtime::FrameTelemetrySample latest{};
    if (!collector->latestSample(latest)) return;

    tooling::writeProtocolMessage(output, tooling::Json::Object{
        {"seq", static_cast<std::int64_t>(sequence_++)},
        {"type", "event"},
        {"event", "realscript/frameTelemetry"},
        {"body", serializeFrameTelemetrySample(latest)},
    });
    output.flush();
}

tooling::Json DapServer::response(
    const tooling::Json& request,
    bool success,
    tooling::Json body,
    std::string message) {
    tooling::Json::Object result{
        {"seq", static_cast<std::int64_t>(sequence_++)},
        {"type", "response"},
        {"request_seq", request.find("seq") ? request.find("seq")->integerValue() : 0},
        {"success", success},
        {"command", request.find("command") ? request.find("command")->stringValue() : ""},
        {"body", std::move(body)},
    };
    if (!message.empty()) result.emplace("message", std::move(message));
    return result;
}

std::string DapServer::describeValueType(
    const runtime::Value& value,
    semantic::PrimitiveType fallback) const {
    if (const auto* objRef = std::get_if<runtime::ObjectRef>(&value)) {
        if (heap_ && objRef->valid() && heap_->isAlive(*objRef)) {
            if (objRef->kind == runtime::ObjectKind::Array) {
                const auto elemType = heap_->arrayElementType(*objRef).value_or(semantic::PrimitiveType::Error);
                const auto elemTypeId = heap_->arrayElementTypeId(*objRef).value_or(0);
                if (program_ && elemTypeId != 0) {
                    if (const auto* elemDesc = program_->resolveType(elemTypeId)) {
                        return elemDesc->name + "[]";
                    }
                }
                if (elemType != semantic::PrimitiveType::Error) {
                    return std::string(semantic::primitiveTypeName(elemType)) + "[]";
                }
                return "array";
            }
            const auto typeId = heap_->objectTypeId(*objRef).value_or(0);
            if (program_ && typeId != 0) {
                if (const auto* typeDesc = program_->resolveType(typeId)) {
                    return typeDesc->name.empty()
                        ? semantic::canonicalTypeName(*typeDesc)
                        : typeDesc->name;
                }
            }
        }
    } else if (const auto* stVal = std::get_if<runtime::StructValue>(&value)) {
        if (program_ && stVal->typeId != 0) {
            if (const auto* typeDesc = program_->resolveType(stVal->typeId)) {
                return typeDesc->name.empty()
                    ? semantic::canonicalTypeName(*typeDesc)
                    : typeDesc->name;
            }
        }
    } else if (std::holds_alternative<runtime::Vec2Value>(value)) {
        return "Vec2";
    } else if (std::holds_alternative<runtime::Vec3Value>(value)) {
        return "Vec3";
    } else if (std::holds_alternative<runtime::QuatValue>(value)) {
        return "Quat";
    } else if (std::holds_alternative<runtime::FixedValue>(value)) {
        return "Fixed";
    } else if (std::holds_alternative<runtime::Fixed16Value>(value)) {
        return "Fixed16";
    } else if (std::holds_alternative<runtime::Fixed64Value>(value)) {
        return "Fixed64";
    } else if (std::holds_alternative<runtime::Vec2fValue>(value)) {
        return "Vec2f";
    } else if (std::holds_alternative<runtime::Vec3fValue>(value)) {
        return "Vec3f";
    } else if (std::holds_alternative<runtime::QuatfValue>(value)) {
        return "Quatf";
    }
    const auto p = fallback != semantic::PrimitiveType::Error
        ? fallback
        : runtime::valueType(value);
    return std::string(semantic::primitiveTypeName(p));
}

std::int64_t DapServer::registerExpandableValue(
    const runtime::Value& value,
    std::uint32_t ownerFrameId) {
    const auto bindRefFrame = [&](std::int64_t refId) {
        if (ownerFrameId != 0) {
            variableFrameIds_[refId] = ownerFrameId;
        }
        return refId;
    };
    if (const auto* objRef = std::get_if<runtime::ObjectRef>(&value)) {
        if (!heap_ || !objRef->valid() || !heap_->isAlive(*objRef)) return 0;
        if (objRef->kind == runtime::ObjectKind::String) return 0;
        if (objRef->kind == runtime::ObjectKind::Array) {
            const auto len = heap_->arrayLength(*objRef).value_or(0);
            const auto elemType = heap_->arrayElementType(*objRef).value_or(semantic::PrimitiveType::Error);
            const auto elemTypeId = heap_->arrayElementTypeId(*objRef).value_or(0);
            std::vector<DebugVariable> children;
            children.reserve(std::min<std::size_t>(len, 256));
            for (std::size_t i = 0; i < std::min<std::size_t>(len, 256); ++i) {
                if (const auto elem = heap_->arrayGet(*objRef, i)) {
                    children.push_back(DebugVariable{
                        "[" + std::to_string(i) + "]",
                        *elem,
                        elemType != semantic::PrimitiveType::Error ? elemType : runtime::valueType(*elem),
                        elemTypeId,
                    });
                }
            }
            const auto refId = nextChildRef_++;
            variables_[refId] = std::move(children);
            childOwnerObjects_[refId] = *objRef;
            return bindRefFrame(refId);
        }
        const auto typeId = heap_->objectTypeId(*objRef).value_or(0);
        const auto* typeDesc = (program_ && typeId != 0) ? program_->resolveType(typeId) : nullptr;
        const auto fCount = heap_->fieldCount(*objRef).value_or(0);
        std::vector<DebugVariable> children;

        // Logical collection preview for List / Queue / Stack / HashSet / Dictionary
        if (typeDesc) {
            auto getNamedField = [&](std::string_view fname, std::size_t fallbackIdx) -> std::optional<runtime::Value> {
                for (std::size_t i = 0; i < typeDesc->fields.size(); ++i) {
                    const auto& fld = typeDesc->fields[i];
                    if (fld.staticField || fld.constField) continue;
                    if (fld.name == fname) {
                        const std::size_t slotIdx = fld.index < fCount ? fld.index : i;
                        return heap_->fieldGet(*objRef, slotIdx);
                    }
                }
                return fallbackIdx < fCount ? heap_->fieldGet(*objRef, fallbackIdx) : std::nullopt;
            };
            const auto isBackingArrayOrNull = [&](const runtime::Value& v, const runtime::ObjectRef*& outArr) noexcept {
                if (const auto* arr = std::get_if<runtime::ObjectRef>(&v);
                    arr && arr->valid() && heap_->isAlive(*arr)) {
                    outArr = arr;
                    return true;
                }
                outArr = nullptr;
                return std::holds_alternative<runtime::NullArray>(v) ||
                    std::holds_alternative<runtime::NullObject>(v);
            };
            const auto& tName = typeDesc->name;
            const bool isHashSet = (tName.rfind("HashSet__", 0) == 0);
            if ((tName.rfind("List__", 0) == 0 || isHashSet) &&
                fCount >= 2) {
                const auto itemsVal = isHashSet
                    ? getNamedField("values", 1)
                    : getNamedField("items", 0);
                const auto countVal = getNamedField("count", isHashSet ? (fCount - 1) : 1);
                const runtime::ObjectRef* arrRef = nullptr;
                if (itemsVal && countVal && std::holds_alternative<std::int64_t>(*countVal) &&
                    isBackingArrayOrNull(*itemsVal, arrRef)) {
                    const auto logicalCount = static_cast<std::size_t>(
                        std::max<std::int64_t>(0, std::get<std::int64_t>(*countVal)));
                    children.push_back(DebugVariable{"Count", *countVal, semantic::PrimitiveType::Int, 0});
                    if (arrRef) {
                        const auto elemType = heap_->arrayElementType(*arrRef).value_or(semantic::PrimitiveType::Error);
                        const auto elemTypeId = heap_->arrayElementTypeId(*arrRef).value_or(0);
                        for (std::size_t i = 0; i < std::min<std::size_t>(logicalCount, 256); ++i) {
                            if (const auto elem = heap_->arrayGet(*arrRef, i)) {
                                children.push_back(DebugVariable{
                                    "[" + std::to_string(i) + "]",
                                    *elem,
                                    elemType != semantic::PrimitiveType::Error ? elemType : runtime::valueType(*elem),
                                    elemTypeId,
                                });
                            }
                        }
                    }
                    const auto refId = nextChildRef_++;
                    variables_[refId] = std::move(children);
                    childOwnerObjects_[refId] = *objRef;
                    return bindRefFrame(refId);
                }
            } else if (tName.rfind("Stack__", 0) == 0 && fCount >= 2) {
                const auto itemsVal = getNamedField("items", 0);
                const auto countVal = getNamedField("count", 1);
                const runtime::ObjectRef* arrRef = nullptr;
                if (itemsVal && countVal && std::holds_alternative<std::int64_t>(*countVal) &&
                    isBackingArrayOrNull(*itemsVal, arrRef)) {
                    const auto logicalCount = static_cast<std::size_t>(
                        std::max<std::int64_t>(0, std::get<std::int64_t>(*countVal)));
                    children.push_back(DebugVariable{"Count", *countVal, semantic::PrimitiveType::Int, 0});
                    if (arrRef && logicalCount > 0) {
                        const auto elemType = heap_->arrayElementType(*arrRef).value_or(semantic::PrimitiveType::Error);
                        const auto elemTypeId = heap_->arrayElementTypeId(*arrRef).value_or(0);
                        const auto limit = std::min<std::size_t>(logicalCount, 256);
                        for (std::size_t i = 0; i < limit; ++i) {
                            const std::size_t physIdx = logicalCount - 1 - i;
                            if (const auto elem = heap_->arrayGet(*arrRef, physIdx)) {
                                children.push_back(DebugVariable{
                                    "[" + std::to_string(i) + "]",
                                    *elem,
                                    elemType != semantic::PrimitiveType::Error ? elemType : runtime::valueType(*elem),
                                    elemTypeId,
                                });
                            }
                        }
                    }
                    const auto refId = nextChildRef_++;
                    variables_[refId] = std::move(children);
                    childOwnerObjects_[refId] = *objRef;
                    return bindRefFrame(refId);
                }
            } else if (tName.rfind("Queue__", 0) == 0 && fCount >= 4) {
                const auto itemsVal = getNamedField("items", 0);
                const auto headVal = getNamedField("head", 1);
                const auto countVal = getNamedField("count", 3);
                const runtime::ObjectRef* arrRef = nullptr;
                if (itemsVal && headVal && countVal &&
                    std::holds_alternative<std::int64_t>(*headVal) &&
                    std::holds_alternative<std::int64_t>(*countVal) &&
                    isBackingArrayOrNull(*itemsVal, arrRef)) {
                    const auto logicalCount = static_cast<std::size_t>(
                        std::max<std::int64_t>(0, std::get<std::int64_t>(*countVal)));
                    const auto headIdx = static_cast<std::size_t>(
                        std::max<std::int64_t>(0, std::get<std::int64_t>(*headVal)));
                    children.push_back(DebugVariable{"Count", *countVal, semantic::PrimitiveType::Int, 0});
                    if (arrRef) {
                        const auto arrLen = heap_->arrayLength(*arrRef).value_or(0);
                        const auto elemType = heap_->arrayElementType(*arrRef).value_or(semantic::PrimitiveType::Error);
                        const auto elemTypeId = heap_->arrayElementTypeId(*arrRef).value_or(0);
                        if (arrLen > 0) {
                            const auto limit = std::min<std::size_t>(logicalCount, 256);
                            for (std::size_t i = 0; i < limit; ++i) {
                                const std::size_t physIdx = (headIdx + i) % arrLen;
                                if (const auto elem = heap_->arrayGet(*arrRef, physIdx)) {
                                    children.push_back(DebugVariable{
                                        "[" + std::to_string(i) + "]",
                                        *elem,
                                        elemType != semantic::PrimitiveType::Error ? elemType : runtime::valueType(*elem),
                                        elemTypeId,
                                    });
                                }
                            }
                        }
                    }
                    const auto refId = nextChildRef_++;
                    variables_[refId] = std::move(children);
                    childOwnerObjects_[refId] = *objRef;
                    return bindRefFrame(refId);
                }
            } else if (tName.rfind("Dictionary__", 0) == 0 && fCount >= 5) {
                const auto keysVal = getNamedField("keys", 0);
                const auto valsVal = getNamedField("values", 1);
                const auto countVal = getNamedField("count", 4);
                const runtime::ObjectRef* kArr = nullptr;
                const runtime::ObjectRef* vArr = nullptr;
                if (keysVal && valsVal && countVal && std::holds_alternative<std::int64_t>(*countVal) &&
                    isBackingArrayOrNull(*keysVal, kArr) && isBackingArrayOrNull(*valsVal, vArr)) {
                    const auto logicalCount = static_cast<std::size_t>(
                        std::max<std::int64_t>(0, std::get<std::int64_t>(*countVal)));
                    children.push_back(DebugVariable{"Count", *countVal, semantic::PrimitiveType::Int, 0});
                    if (kArr && vArr) {
                        const auto vElemType = heap_->arrayElementType(*vArr).value_or(semantic::PrimitiveType::Error);
                        const auto vElemTypeId = heap_->arrayElementTypeId(*vArr).value_or(0);
                        for (std::size_t i = 0; i < std::min<std::size_t>(logicalCount, 256); ++i) {
                            const auto kElem = heap_->arrayGet(*kArr, i);
                            const auto vElem = heap_->arrayGet(*vArr, i);
                            if (kElem && vElem) {
                                children.push_back(DebugVariable{
                                    "[" + runtime::valueToString(*kElem, heap_.get()) + "]",
                                    *vElem,
                                    vElemType != semantic::PrimitiveType::Error ? vElemType : runtime::valueType(*vElem),
                                    vElemTypeId,
                                });
                            }
                        }
                    }
                    const auto refId = nextChildRef_++;
                    variables_[refId] = std::move(children);
                    childOwnerObjects_[refId] = *objRef;
                    return bindRefFrame(refId);
                }
            }
        }

        for (std::size_t i = 0; i < fCount; ++i) {
            if (typeDesc && i < typeDesc->fields.size()) {
                if (typeDesc->fields[i].staticField ||
                    typeDesc->fields[i].constField ||
                    typeDesc->fields[i].synthetic) {
                    continue;
                }
            }
            const auto fVal = heap_->fieldGet(*objRef, i);
            if (!fVal) continue;
            std::string fName = "field_" + std::to_string(i);
            semantic::PrimitiveType fType = runtime::valueType(*fVal);
            if (typeDesc && i < typeDesc->fields.size()) {
                fName = typeDesc->fields[i].name;
                fType = typeDesc->fields[i].type;
            }
            children.push_back(DebugVariable{std::move(fName), *fVal, fType, 0});
        }
        if (children.empty()) return 0;
        const auto refId = nextChildRef_++;
        variables_[refId] = std::move(children);
        childOwnerObjects_[refId] = *objRef;
        return bindRefFrame(refId);
    }
    if (const auto* stVal = std::get_if<runtime::StructValue>(&value)) {
        if (!stVal->valid() || stVal->fieldCount() == 0) return 0;
        const auto* typeDesc = (program_ && stVal->typeId != 0) ? program_->resolveType(stVal->typeId) : nullptr;
        std::vector<DebugVariable> children;
        for (std::size_t i = 0; i < stVal->fieldCount(); ++i) {
            if (typeDesc && i < typeDesc->fields.size()) {
                if (typeDesc->fields[i].staticField ||
                    typeDesc->fields[i].constField ||
                    typeDesc->fields[i].synthetic) {
                    continue;
                }
            }
            const auto fVal = runtime::structValueFieldAt(*stVal, i);
            if (!fVal) continue;
            std::string fName = "field_" + std::to_string(i);
            semantic::PrimitiveType fType = runtime::valueType(*fVal);
            if (typeDesc && i < typeDesc->fields.size()) {
                fName = typeDesc->fields[i].name;
                fType = typeDesc->fields[i].type;
            }
            children.push_back(DebugVariable{std::move(fName), *fVal, fType, 0});
        }
        if (children.empty()) return 0;
        const auto refId = nextChildRef_++;
        variables_[refId] = std::move(children);
        return bindRefFrame(refId);
    }
    if (const auto* v2 = std::get_if<runtime::Vec2Value>(&value)) {
        const auto refId = nextChildRef_++;
        variables_[refId] = {
            DebugVariable{"x", runtime::Value(runtime::FixedValue{v2->x}), semantic::PrimitiveType::Struct, 0},
            DebugVariable{"y", runtime::Value(runtime::FixedValue{v2->y}), semantic::PrimitiveType::Struct, 0},
        };
        return bindRefFrame(refId);
    }
    if (const auto* v3 = std::get_if<runtime::Vec3Value>(&value)) {
        const auto refId = nextChildRef_++;
        variables_[refId] = {
            DebugVariable{"x", runtime::Value(runtime::FixedValue{v3->x}), semantic::PrimitiveType::Struct, 0},
            DebugVariable{"y", runtime::Value(runtime::FixedValue{v3->y}), semantic::PrimitiveType::Struct, 0},
            DebugVariable{"z", runtime::Value(runtime::FixedValue{v3->z}), semantic::PrimitiveType::Struct, 0},
        };
        return bindRefFrame(refId);
    }
    if (const auto* q = std::get_if<runtime::QuatValue>(&value)) {
        const auto refId = nextChildRef_++;
        variables_[refId] = {
            DebugVariable{"x", runtime::Value(runtime::FixedValue{q->x}), semantic::PrimitiveType::Struct, 0},
            DebugVariable{"y", runtime::Value(runtime::FixedValue{q->y}), semantic::PrimitiveType::Struct, 0},
            DebugVariable{"z", runtime::Value(runtime::FixedValue{q->z}), semantic::PrimitiveType::Struct, 0},
            DebugVariable{"w", runtime::Value(runtime::FixedValue{q->w}), semantic::PrimitiveType::Struct, 0},
        };
        return bindRefFrame(refId);
    }
    return 0;
}

void DapServer::rebuildVariableReferences() {
    variables_.clear();
    childOwnerObjects_.clear();
    variableFrameIds_.clear();
    nextChildRef_ = kFirstChildVariablesReference;
    const auto stop = session_->currentStop();
    if (!stop) return;
    for (const auto& frame : stop->frames) {
        const auto argsRef = static_cast<std::int64_t>(frame.id) * 2;
        const auto localsRef = static_cast<std::int64_t>(frame.id) * 2 + 1;
        variables_[argsRef] = frame.arguments;
        variables_[localsRef] = frame.locals;
        variableFrameIds_[argsRef] = frame.id;
        variableFrameIds_[localsRef] = frame.id;
    }
}

void DapServer::emitStopped(std::ostream& output) {
    rebuildVariableReferences();
    const auto stop = session_->currentStop();
    if (!stop) return;
    tooling::Json::Object bodyObj{
        {"reason", stopReasonName(stop->reason)},
        {"threadId", 1},
        {"allThreadsStopped", true},
        {"hitBreakpointIds", stop->breakpointId == 0
            ? tooling::Json(tooling::Json::Array{})
            : tooling::Json(tooling::Json::Array{static_cast<std::int64_t>(stop->breakpointId)})},
    };
    if (stop->reason == StopReason::Exception) {
        bodyObj.emplace("text", stop->exceptionDescription);
        bodyObj.emplace(
            "description",
            stop->exceptionDescription.empty()
                ? std::string("Paused on exception")
                : stop->exceptionDescription);
    }
    tooling::writeProtocolMessage(output, tooling::Json::Object{
        {"seq", static_cast<std::int64_t>(sequence_++)},
        {"type", "event"},
        {"event", "stopped"},
        {"body", std::move(bodyObj)},
    });
    output.flush();
}

void DapServer::emitTerminated(std::ostream& output) {
    tooling::writeProtocolMessage(output, tooling::Json::Object{
        {"seq", static_cast<std::int64_t>(sequence_++)},
        {"type", "event"},
        {"event", "terminated"},
        {"body", tooling::Json::Object{}},
    });
    output.flush();
}

void DapServer::collectActiveStackValues(std::vector<runtime::Value>& outValues) const {
    outValues.clear();
    std::optional<DebugStop> stop = session_ ? session_->currentStop() : std::nullopt;
    if (!stop && session_ && session_->controller()) {
        stop = session_->controller()->lastStop();
    }
    if (stop) {
        for (const auto& frame : stop->frames) {
            for (const auto& arg : frame.arguments) outValues.push_back(arg.value);
            for (const auto& loc : frame.locals) outValues.push_back(loc.value);
        }
    }
}

std::string DapServer::describeHeapObject(
    const runtime::HeapObjectInfo& obj) const {
    std::ostringstream os;
    os << "o" << obj.reference.slot << ":" << obj.reference.generation << " ";
    if (obj.kind == runtime::ObjectKind::String) {
        std::string preview = obj.stringValue;
        if (preview.size() > 36) {
            preview = preview.substr(0, 33) + "...";
        }
        os << "string(\"" << preview << "\")";
    } else if (obj.kind == runtime::ObjectKind::Array) {
        std::string elemName = semantic::primitiveTypeName(obj.elementType);
        if (program_ && obj.elementTypeId != 0) {
            if (const auto* elemTypeDesc = program_->resolveType(obj.elementTypeId)) {
                elemName = semantic::canonicalTypeName(*elemTypeDesc);
            }
        }
        os << elemName << "[" << obj.valueCount << "]";
    } else {
        std::string typeName = "Record";
        if (program_ && obj.typeId != 0) {
            if (const auto* typeDesc = program_->resolveType(obj.typeId)) {
                typeName = semantic::canonicalTypeName(*typeDesc);
            }
        }
        os << typeName;
    }
    os << " (" << obj.sizeBytes << " B)";
    return os.str();
}

std::string DapServer::describeEdgeLabel(
    const runtime::HeapObjectInfo& owner,
    const std::string& rawLabel) const {
    if (owner.kind == runtime::ObjectKind::Record &&
        program_ && owner.typeId != 0 &&
        rawLabel.rfind("field[", 0) == 0) {
        const auto closePos = rawLabel.find(']');
        if (closePos != std::string::npos) {
            const auto fieldIdx = static_cast<std::size_t>(
                std::strtoull(rawLabel.substr(6, closePos - 6).c_str(), nullptr, 10));
            if (const auto* typeDesc = program_->resolveType(owner.typeId)) {
                if (fieldIdx < typeDesc->fields.size()) {
                    const std::string suffix = rawLabel.substr(closePos + 1);
                    return "." + typeDesc->fields[fieldIdx].name + suffix;
                }
            }
        }
    }
    if (rawLabel.rfind("element[", 0) == 0) {
        return rawLabel.substr(7); // "[i]"
    }
    return "." + rawLabel;
}

std::string DapServer::describeRootInfo(
    const runtime::HeapRootInfo& root) const {
    if (root.kind == "static" && program_) {
        for (const auto& mod : program_->modules()) {
            for (const auto& type : mod.types) {
                for (const auto& fld : type.fields) {
                    if (fld.staticField && fld.id == root.token) {
                        return "[Root: Static " + semantic::canonicalTypeName(type) +
                            "." + fld.name + "]";
                    }
                }
            }
        }
        return "[Root: Static#0x" + std::to_string(root.token) + "]";
    }
    if (root.kind == "persistent") {
        return "[Root: Persistent#" + std::to_string(root.token) + "]";
    }
    if (root.kind == "ecs") {
        return "[Root: ECS#" + std::to_string(root.token) + "]";
    }
    if (root.kind == "shadow") {
        std::optional<DebugStop> stop = session_ ? session_->currentStop() : std::nullopt;
        if (!stop && session_ && session_->controller()) {
            stop = session_->controller()->lastStop();
        }
        if (stop) {
            const auto valueContainsRef = [](const auto& self,
                                             const runtime::Value& val,
                                             runtime::ObjectRef target,
                                             std::unordered_set<const runtime::StructStorage*>& visited) -> bool {
                if (const auto* ref = std::get_if<runtime::ObjectRef>(&val)) {
                    return *ref == target;
                }
                if (const auto* st = std::get_if<runtime::StructValue>(&val)) {
                    if (st->isFrameSlice()) {
                        for (std::size_t fi = 0; fi < st->fieldCount(); ++fi) {
                            if (const auto fv = runtime::structValueFieldAt(*st, fi);
                                fv && self(self, *fv, target, visited)) {
                                return true;
                            }
                        }
                    } else if (st->storage && visited.insert(st->storage.get()).second) {
                        for (const auto& f : st->storage->fields) {
                            if (self(self, f, target, visited)) return true;
                        }
                    }
                }
                return false;
            };
            std::size_t idx = 0;
            for (const auto& frame : stop->frames) {
                for (const auto& arg : frame.arguments) {
                    std::unordered_set<const runtime::StructStorage*> visited;
                    if (idx == root.token ||
                        valueContainsRef(valueContainsRef, arg.value, root.reference, visited)) {
                        return "[Root: Frame " + frame.function + " arg '" + arg.name + "']";
                    }
                    ++idx;
                }
                for (const auto& loc : frame.locals) {
                    std::unordered_set<const runtime::StructStorage*> visited;
                    if (idx == root.token ||
                        valueContainsRef(valueContainsRef, loc.value, root.reference, visited)) {
                        return "[Root: Frame " + frame.function + " local '" + loc.name + "']";
                    }
                    ++idx;
                }
            }
        }
        return "[Root: Stack#" + std::to_string(root.token) + "]";
    }
    return "[Root: " + root.kind + "#" + std::to_string(root.token) + "]";
}

std::string DapServer::formatRetainingPathReport(runtime::ObjectRef target) const {
    if (!heap_ || !target.valid()) {
        return "Target object is not alive in ManagedHeap.";
    }
    bool alive = false;
    runtime::HeapSnapshot snap;
    std::vector<std::string> rootDescriptions;
    const auto captureSnap = [&]() {
        if (!heap_->isAlive(target)) return;
        alive = true;
        std::vector<runtime::Value> stackValues;
        collectActiveStackValues(stackValues);
        runtime::ShadowStack shadow;
        shadow.pushFrame(nullptr, nullptr, &stackValues);
        snap = heap_->snapshot(&shadow);
        rootDescriptions.reserve(snap.roots.size());
        for (const auto& root : snap.roots) {
            rootDescriptions.push_back(describeRootInfo(root));
        }
    };
    if (session_) {
        session_->runAtSafepoint(captureSnap);
    } else {
        captureSnap();
    }
    if (!alive) {
        return "Target object is not alive in ManagedHeap.";
    }
    std::unordered_map<std::uint32_t, const runtime::HeapObjectInfo*> objMap;
    objMap.reserve(snap.objects.size());
    for (const auto& obj : snap.objects) {
        objMap.emplace(obj.reference.slot, &obj);
    }

    struct ParentStep {
        std::uint32_t parentSlot = runtime::ObjectRef::InvalidSlot;
        std::string edgeText;
        std::string rootText;
    };
    std::deque<runtime::ObjectRef> queue;
    std::unordered_map<std::uint32_t, ParentStep> parent;
    std::unordered_set<std::uint32_t> visited;

    for (std::size_t i = 0; i < snap.roots.size(); ++i) {
        const auto& root = snap.roots[i];
        if (visited.insert(root.reference.slot).second) {
            queue.push_back(root.reference);
            parent.emplace(
                root.reference.slot,
                ParentStep{
                    runtime::ObjectRef::InvalidSlot,
                    {},
                    i < rootDescriptions.size() ? rootDescriptions[i] : describeRootInfo(root)});
        }
    }

    bool foundTarget = false;
    while (!queue.empty()) {
        const auto curr = queue.front();
        queue.pop_front();
        if (curr.slot == target.slot) {
            foundTarget = true;
            break;
        }
        const auto it = objMap.find(curr.slot);
        if (it == objMap.end()) continue;
        const auto& ownerInfo = *it->second;
        for (const auto& edge : ownerInfo.edges) {
            if (visited.insert(edge.target.slot).second) {
                parent.emplace(
                    edge.target.slot,
                    ParentStep{curr.slot, describeEdgeLabel(ownerInfo, edge.label), {}});
                queue.push_back(edge.target);
            }
        }
    }

    if (!foundTarget) {
        const auto targetIt = objMap.find(target.slot);
        const std::string targetDesc = targetIt != objMap.end()
            ? describeHeapObject(*targetIt->second)
            : ("o" + std::to_string(target.slot) + ":" + std::to_string(target.generation));
        return "[Unrooted / Pending GC Sweep] " + targetDesc;
    }

    std::vector<std::uint32_t> chainSlots;
    std::uint32_t cursor = target.slot;
    while (cursor != runtime::ObjectRef::InvalidSlot) {
        chainSlots.push_back(cursor);
        const auto pIt = parent.find(cursor);
        if (pIt == parent.end()) break;
        cursor = pIt->second.parentSlot;
    }
    std::reverse(chainSlots.begin(), chainSlots.end());

    std::ostringstream report;
    const auto firstParentIt = parent.find(chainSlots.front());
    if (firstParentIt != parent.end() && !firstParentIt->second.rootText.empty()) {
        report << firstParentIt->second.rootText << " -> ";
    }
    for (std::size_t i = 0; i < chainSlots.size(); ++i) {
        const auto slotId = chainSlots[i];
        const auto objIt = objMap.find(slotId);
        if (objIt != objMap.end()) {
            report << describeHeapObject(*objIt->second);
        } else {
            report << "o" << slotId;
        }
        if (i + 1 < chainSlots.size()) {
            const auto nextSlot = chainSlots[i + 1];
            const auto nextParentIt = parent.find(nextSlot);
            if (nextParentIt != parent.end()) {
                report << " --(" << nextParentIt->second.edgeText << ")--> ";
            } else {
                report << " -> ";
            }
        }
    }
    return report.str();
}

tooling::Json DapServer::buildHeapSnapshotReport(bool diffBaseline) {
    if (!heap_) {
        return tooling::Json::Object{
            {"summaryText", "No ManagedHeap bound to DapServer."},
        };
    }

    runtime::HeapSnapshot snap;
    std::vector<std::string> rootDescriptions;
    const auto captureSnap = [&]() {
        std::vector<runtime::Value> stackValues;
        collectActiveStackValues(stackValues);
        runtime::ShadowStack shadow;
        shadow.pushFrame(nullptr, nullptr, &stackValues);
        snap = heap_->snapshot(&shadow);
        rootDescriptions.reserve(snap.roots.size());
        for (const auto& root : snap.roots) {
            rootDescriptions.push_back(describeRootInfo(root));
        }
    };
    if (session_) {
        session_->runAtSafepoint(captureSnap);
    } else {
        captureSnap();
    }

    // Precompute all shortest retaining paths in a single O(V + E) multi-source BFS over `snap`
    std::unordered_map<std::uint32_t, const runtime::HeapObjectInfo*> objMap;
    objMap.reserve(snap.objects.size());
    for (const auto& obj : snap.objects) {
        objMap.emplace(obj.reference.slot, &obj);
    }
    struct ParentStep {
        std::uint32_t parentSlot = runtime::ObjectRef::InvalidSlot;
        std::string edgeText;
        std::string rootText;
    };
    std::deque<runtime::ObjectRef> bfsQueue;
    std::unordered_map<std::uint32_t, ParentStep> parent;
    std::unordered_set<std::uint32_t> visited;
    parent.reserve(snap.objects.size());
    visited.reserve(snap.objects.size());

    for (std::size_t i = 0; i < snap.roots.size(); ++i) {
        const auto& root = snap.roots[i];
        if (visited.insert(root.reference.slot).second) {
            bfsQueue.push_back(root.reference);
            parent.emplace(
                root.reference.slot,
                ParentStep{
                    runtime::ObjectRef::InvalidSlot,
                    {},
                    i < rootDescriptions.size() ? rootDescriptions[i] : describeRootInfo(root)});
        }
    }
    while (!bfsQueue.empty()) {
        const auto curr = bfsQueue.front();
        bfsQueue.pop_front();
        const auto it = objMap.find(curr.slot);
        if (it == objMap.end()) continue;
        const auto& ownerInfo = *it->second;
        for (const auto& edge : ownerInfo.edges) {
            if (visited.insert(edge.target.slot).second) {
                parent.emplace(
                    edge.target.slot,
                    ParentStep{curr.slot, describeEdgeLabel(ownerInfo, edge.label), {}});
                bfsQueue.push_back(edge.target);
            }
        }
    }

    const auto formatPathFromBfs = [&](const runtime::HeapObjectInfo& targetObj) -> std::string {
        if (visited.find(targetObj.reference.slot) == visited.end()) {
            return "[Unrooted / Pending GC Sweep] " + describeHeapObject(targetObj);
        }
        std::vector<std::uint32_t> chainSlots;
        std::uint32_t cursor = targetObj.reference.slot;
        while (cursor != runtime::ObjectRef::InvalidSlot) {
            chainSlots.push_back(cursor);
            const auto pIt = parent.find(cursor);
            if (pIt == parent.end()) break;
            cursor = pIt->second.parentSlot;
        }
        std::reverse(chainSlots.begin(), chainSlots.end());
        std::ostringstream report;
        const auto firstParentIt = parent.find(chainSlots.front());
        if (firstParentIt != parent.end() && !firstParentIt->second.rootText.empty()) {
            report << firstParentIt->second.rootText << " -> ";
        }
        for (std::size_t i = 0; i < chainSlots.size(); ++i) {
            const auto slotId = chainSlots[i];
            const auto objIt = objMap.find(slotId);
            if (objIt != objMap.end()) {
                report << describeHeapObject(*objIt->second);
            } else {
                report << "o" << slotId;
            }
            if (i + 1 < chainSlots.size()) {
                const auto nextSlot = chainSlots[i + 1];
                const auto nextParentIt = parent.find(nextSlot);
                if (nextParentIt != parent.end()) {
                    report << " --(" << nextParentIt->second.edgeText << ")--> ";
                } else {
                    report << " -> ";
                }
            }
        }
        return report.str();
    };

    std::unordered_set<std::uint64_t> baselineObjectKeys;
    const bool hasPriorBaseline =
        diffBaseline && baselineSnapshot_.has_value() && baselineSnapshot_->heapId == snap.heapId;
    if (hasPriorBaseline) {
        for (const auto& oldObj : baselineSnapshot_->objects) {
            const std::uint64_t key =
                (static_cast<std::uint64_t>(oldObj.reference.slot) << 32u) |
                static_cast<std::uint64_t>(oldObj.reference.generation);
            baselineObjectKeys.insert(key);
        }
    }

    struct TypeHistogramEntry {
        std::string typeName;
        std::size_t count = 0;
        std::size_t totalBytes = 0;
        std::size_t deltaCount = 0;
        std::size_t deltaBytes = 0;
    };
    std::unordered_map<std::string, TypeHistogramEntry> histogramMap;

    tooling::Json::Array objectsJson;
    std::vector<std::string> retainingReports;
    std::size_t newObjectsCount = 0;
    std::size_t newObjectsBytes = 0;

    for (const auto& obj : snap.objects) {
        const std::uint64_t key =
            (static_cast<std::uint64_t>(obj.reference.slot) << 32u) |
            static_cast<std::uint64_t>(obj.reference.generation);
        const bool isNewSinceBaseline =
            !hasPriorBaseline || (baselineObjectKeys.find(key) == baselineObjectKeys.end());

        std::string bucketName;
        if (obj.kind == runtime::ObjectKind::String) {
            bucketName = "string";
        } else if (obj.kind == runtime::ObjectKind::Array) {
            bucketName = std::string(semantic::primitiveTypeName(obj.elementType)) + "[]";
            if (program_ && obj.elementTypeId != 0) {
                if (const auto* elemType = program_->resolveType(obj.elementTypeId)) {
                    bucketName = semantic::canonicalTypeName(*elemType) + "[]";
                }
            }
        } else {
            bucketName = "Record";
            if (program_ && obj.typeId != 0) {
                if (const auto* typeDesc = program_->resolveType(obj.typeId)) {
                    bucketName = semantic::canonicalTypeName(*typeDesc);
                }
            }
        }

        auto& hist = histogramMap[bucketName];
        hist.typeName = bucketName;
        hist.count += 1;
        hist.totalBytes += obj.sizeBytes;
        if (isNewSinceBaseline) {
            hist.deltaCount += 1;
            hist.deltaBytes += obj.sizeBytes;
            ++newObjectsCount;
            newObjectsBytes += obj.sizeBytes;
        }

        const std::string chainStr = formatPathFromBfs(obj);
        if (isNewSinceBaseline && obj.kind != runtime::ObjectKind::String &&
            retainingReports.size() < 50) {
            retainingReports.push_back(chainStr);
        } else if (isNewSinceBaseline && retainingReports.size() < 20) {
            retainingReports.push_back(chainStr);
        }

        objectsJson.emplace_back(tooling::Json::Object{
            {"slot", static_cast<std::int64_t>(obj.reference.slot)},
            {"generation", static_cast<std::int64_t>(obj.reference.generation)},
            {"typeName", bucketName},
            {"sizeBytes", static_cast<std::int64_t>(obj.sizeBytes)},
            {"isNewSinceBaseline", isNewSinceBaseline},
            {"retainingPath", chainStr},
        });
    }

    std::vector<TypeHistogramEntry> sortedHistogram;
    sortedHistogram.reserve(histogramMap.size());
    for (auto& [k, v] : histogramMap) {
        (void)k;
        sortedHistogram.push_back(std::move(v));
    }
    std::sort(
        sortedHistogram.begin(),
        sortedHistogram.end(),
        [](const TypeHistogramEntry& a, const TypeHistogramEntry& b) {
            if (a.totalBytes != b.totalBytes) return a.totalBytes > b.totalBytes;
            return a.typeName < b.typeName;
        });

    tooling::Json::Array histogramJson;
    for (const auto& h : sortedHistogram) {
        histogramJson.emplace_back(tooling::Json::Object{
            {"typeName", h.typeName},
            {"count", static_cast<std::int64_t>(h.count)},
            {"bytes", static_cast<std::int64_t>(h.totalBytes)},
            {"deltaCount", static_cast<std::int64_t>(h.deltaCount)},
            {"deltaBytes", static_cast<std::int64_t>(h.deltaBytes)},
        });
    }

    std::ostringstream md;
    md << "# RealScript ManagedHeap Memory Snapshot & Retaining Path Report\n\n";
    md << "## 1. Heap Summary\n";
    md << "- **Heap ID**: `" << snap.heapId << "`\n";
    md << "- **Live Objects**: `" << snap.objects.size() << "` (`" << snap.statistics.liveBytes << " B` / Peak `"
       << snap.statistics.peakLiveBytes << " B`)\n";
    md << "- **Active Roots**: `" << snap.roots.size()
       << "` (Persistent / Static / ECS / Stack)\n";
    md << "- **GC Collections**: Full `" << snap.statistics.collectionsCompleted
       << "`, Young `" << snap.statistics.youngCollections
       << "` (Reclaimed `" << snap.statistics.objectsReclaimed << "` objects)\n";
    if (hasPriorBaseline) {
        md << "- **Delta vs. Previous Snapshot**: `+" << newObjectsCount
           << "` newly retained objects (`+" << newObjectsBytes << " B`)\n";
    }
    md << "\n## 2. Live Object Histogram by Type\n\n";
    md << "| Type / Category | Live Count | Live Bytes | New Count (Delta) | New Bytes (Delta) |\n";
    md << "|---|---:|---:|---:|---:|\n";
    for (const auto& h : sortedHistogram) {
        md << "| `" << h.typeName << "` | " << h.count << " | " << h.totalBytes << " B | +"
           << h.deltaCount << " | +" << h.deltaBytes << " B |\n";
    }

    md << "\n## 3. Retaining Paths (Root -> Object Reference Chains)\n\n";
    if (retainingReports.empty()) {
        md << "_No live or newly retained objects found._\n";
    } else {
        for (const auto& chain : retainingReports) {
            md << "- `" << chain << "`\n";
        }
    }

    baselineSnapshot_ = snap;

    return tooling::Json::Object{
        {"heapId", static_cast<std::int64_t>(snap.heapId)},
        {"liveObjects", static_cast<std::int64_t>(snap.objects.size())},
        {"liveBytes", static_cast<std::int64_t>(snap.statistics.liveBytes)},
        {"peakLiveBytes", static_cast<std::int64_t>(snap.statistics.peakLiveBytes)},
        {"rootCount", static_cast<std::int64_t>(snap.roots.size())},
        {"hasBaselineDiff", hasPriorBaseline},
        {"newObjectsCount", static_cast<std::int64_t>(newObjectsCount)},
        {"newObjectsBytes", static_cast<std::int64_t>(newObjectsBytes)},
        {"histogram", tooling::Json(std::move(histogramJson))},
        {"objects", tooling::Json(std::move(objectsJson))},
        {"summaryText", md.str()},
    };
}

tooling::Json DapServer::handle(
    const tooling::Json& request,
    std::ostream* events) {
    const auto command = request.find("command")
        ? request.find("command")->stringValue() : std::string{};
    const auto* arguments = request.find("arguments");

    if (command == "initialize") {
        if (events) {
            tooling::writeProtocolMessage(*events, tooling::Json::Object{
                {"seq", static_cast<std::int64_t>(sequence_++)},
                {"type", "event"},
                {"event", "initialized"},
                {"body", tooling::Json::Object{}},
            });
        }
        return response(request, true, tooling::Json::Object{
            {"supportsConfigurationDoneRequest", true},
            {"supportsTerminateRequest", true},
            {"supportsSetVariable", true},
            {"supportsConditionalBreakpoints", true},
            {"supportsEvaluateForHovers", true},
            {"supportTerminateDebuggee", true},
            {"supportsExceptionInfoRequest", true},
            {"exceptionBreakpointFilters", tooling::Json::Array{
                tooling::Json::Object{
                    {"filter", "all"},
                    {"label", "All Exceptions"},
                    {"description", "Break when any script exception is thrown or runtime error occurs"},
                    {"default", false},
                },
                tooling::Json::Object{
                    {"filter", "uncaught"},
                    {"label", "Uncaught Exceptions"},
                    {"description", "Break on uncaught script exceptions and runtime errors"},
                    {"default", true},
                },
                tooling::Json::Object{
                    {"filter", "runtime-error"},
                    {"label", "Runtime Faults"},
                    {"description", "Break on VM runtime faults (NullReference, DivisionByZero, IndexOutOfRange)"},
                    {"default", true},
                },
            }},
        });
    }
    if (command == "setExceptionBreakpoints") {
        bool breakOnAll = false;
        bool breakOnUncaught = false;
        bool breakOnRuntimeError = false;
        if (const auto* filters = arguments ? arguments->find("filters") : nullptr) {
            for (const auto& f : filters->arrayValue()) {
                const auto filterName = f.stringValue();
                if (filterName == "all") {
                    breakOnAll = true;
                    breakOnUncaught = true;
                    breakOnRuntimeError = true;
                } else if (filterName == "uncaught") {
                    breakOnUncaught = true;
                } else if (filterName == "runtime-error") {
                    breakOnRuntimeError = true;
                }
            }
        }
        if (session_ && session_->controller()) {
            session_->controller()->setExceptionBreakpoints(
                breakOnAll, breakOnUncaught, breakOnRuntimeError);
        }
        return response(request, true, tooling::Json::Object{
            {"breakpoints", tooling::Json::Array{}},
        });
    }
    if (command == "exceptionInfo") {
        std::optional<DebugStop> stop = session_ ? session_->currentStop() : std::nullopt;
        if (!stop && session_ && session_->controller()) {
            stop = session_->controller()->lastStop();
        }
        const std::string excId = (stop && !stop->exceptionId.empty())
            ? stop->exceptionId
            : std::string("RuntimeError");
        const std::string desc = stop ? stop->exceptionDescription : std::string{};
        const std::string breakMode = (stop && !stop->exceptionUncaught)
            ? std::string("always")
            : std::string("unhandled");
        return response(request, true, tooling::Json::Object{
            {"exceptionId", excId},
            {"description", desc},
            {"breakMode", breakMode},
            {"details", tooling::Json::Object{
                {"message", desc},
                {"typeName", excId},
            }},
        });
    }
    if (command == "setBreakpoints") {
        const auto path = nested(request, {"arguments", "source", "path"})
            ? nested(request, {"arguments", "source", "path"})->stringValue() : std::string{};
        std::vector<BreakpointSpec> specs;
        if (const auto* values = arguments ? arguments->find("breakpoints") : nullptr) {
            for (const auto& value : values->arrayValue()) {
                const auto line = value.find("line")
                    ? value.find("line")->integerValue(1) : 1;
                const auto column = value.find("column")
                    ? value.find("column")->integerValue(1) : 1;
                const auto condition = value.find("condition")
                    ? value.find("condition")->stringValue() : std::string{};
                specs.push_back(BreakpointSpec{
                    {
                        static_cast<std::size_t>(std::max<std::int64_t>(1, line) - 1),
                        static_cast<std::size_t>(std::max<std::int64_t>(1, column) - 1),
                    },
                    condition,
                });
            }
        }
        tooling::Json::Array breakpoints;
        for (const auto& breakpoint : session_->controller()->setConditionalBreakpoints(path, specs)) {
            if (breakpoint.sourcePath != path) continue;
            breakpoints.emplace_back(tooling::Json::Object{
                {"id", static_cast<std::int64_t>(breakpoint.id)},
                {"verified", breakpoint.verified},
                {"line", static_cast<std::int64_t>(breakpoint.line + 1)},
                {"column", static_cast<std::int64_t>(breakpoint.column + 1)},
            });
        }
        return response(request, true, tooling::Json::Object{{"breakpoints", tooling::Json(std::move(breakpoints))}});
    }
    if (command == "frameTelemetry" || command == "realscript/frameTelemetry") {
        const std::size_t maxFrames = arguments && arguments->find("maxFrames")
            ? static_cast<std::size_t>(std::max<std::int64_t>(1, arguments->find("maxFrames")->integerValue(16)))
            : 16u;
        return response(request, true, buildFrameTelemetryReport(maxFrames));
    }
    if (command == "heapSnapshot" || command == "realscript/heapSnapshot") {
        const bool diffBaseline = arguments && arguments->find("diffBaseline")
            ? arguments->find("diffBaseline")->boolValue(false)
            : false;
        tooling::Json report;
        if (session_) {
            session_->runAtSafepoint([&]() {
                report = buildHeapSnapshotReport(diffBaseline);
            });
        } else {
            report = buildHeapSnapshotReport(diffBaseline);
        }
        return response(request, true, std::move(report));
    }
    if (command == "retainingPath" || command == "realscript/retainingPath") {
        runtime::ObjectRef targetRef;
        bool isAlive = false;
        std::string report;
        const auto resolveTarget = [&]() {
            if (arguments && arguments->find("variablesReference")) {
                const auto varRef = arguments->find("variablesReference")->integerValue(0);
                const auto ownerIt = childOwnerObjects_.find(varRef);
                if (ownerIt != childOwnerObjects_.end()) {
                    targetRef = ownerIt->second;
                }
            }
            if (!targetRef.valid() && arguments && arguments->find("expression") && session_) {
                const auto expr = arguments->find("expression")->stringValue();
                const auto frameId = arguments && arguments->find("frameId")
                    ? static_cast<std::uint32_t>(std::max<std::int64_t>(1, arguments->find("frameId")->integerValue(1)))
                    : 1u;
                std::string errMsg;
                if (const auto evalVal = session_->evaluate(expr, frameId, &errMsg)) {
                    if (const auto* ref = std::get_if<runtime::ObjectRef>(&*evalVal)) {
                        targetRef = *ref;
                    }
                }
            }
            if (!targetRef.valid() && arguments && arguments->find("slot")) {
                const auto slot = static_cast<std::uint32_t>(
                    std::max<std::int64_t>(0, arguments->find("slot")->integerValue(0)));
                if (heap_) {
                    const auto snap = heap_->snapshot();
                    for (const auto& obj : snap.objects) {
                        if (obj.reference.slot == slot) {
                            targetRef = obj.reference;
                            break;
                        }
                    }
                }
            }
            isAlive = targetRef.valid() && heap_ && heap_->isAlive(targetRef);
            if (isAlive) {
                report = formatRetainingPathReport(targetRef);
            }
        };
        if (session_) {
            session_->runAtSafepoint(resolveTarget);
        } else {
            resolveTarget();
        }
        if (!isAlive) {
            return response(request, false, nullptr,
                "target expression or variable does not resolve to a live ManagedHeap object");
        }
        return response(request, true, tooling::Json::Object{
            {"slot", static_cast<std::int64_t>(targetRef.slot)},
            {"generation", static_cast<std::int64_t>(targetRef.generation)},
            {"retainingPath", report},
        });
    }
    if (command == "evaluate") {
        std::string expression = arguments && arguments->find("expression")
            ? arguments->find("expression")->stringValue() : std::string{};
        const auto frameId = arguments && arguments->find("frameId")
            ? static_cast<std::uint32_t>(std::max<std::int64_t>(1, arguments->find("frameId")->integerValue(1)))
            : 1u;
        while (!expression.empty() && std::isspace(static_cast<unsigned char>(expression.front()))) {
            expression.erase(expression.begin());
        }
        while (!expression.empty() && std::isspace(static_cast<unsigned char>(expression.back()))) {
            expression.pop_back();
        }
        if (expression == ":telemetry" || expression.rfind(":telemetry ", 0) == 0) {
            std::string arg = expression.size() > 10 ? expression.substr(10) : std::string{};
            while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.front()))) {
                arg.erase(arg.begin());
            }
            const bool asJson = (arg == "json" || arg.rfind("json ", 0) == 0);
            std::size_t maxFrames = 8u;
            if (!arg.empty() && std::isdigit(static_cast<unsigned char>(arg.front()))) {
                maxFrames = static_cast<std::size_t>(std::max<unsigned long long>(
                    1ULL, std::strtoull(arg.c_str(), nullptr, 10)));
            }
            const runtime::FrameTelemetryCollector* collector =
                frameTelemetry_ ? frameTelemetry_ : runtime::activeFrameTelemetry();
            if (asJson) {
                const std::string jsonStr = collector
                    ? collector->toJson(program_.get(), maxFrames)
                    : buildFrameTelemetryReport(maxFrames).dump();
                return response(request, true, tooling::Json::Object{
                    {"result", jsonStr},
                    {"type", "FrameTelemetryJson"},
                    {"variablesReference", 0},
                });
            }
            const auto rep = buildFrameTelemetryReport(maxFrames);
            return response(request, true, tooling::Json::Object{
                {"result", rep.find("summaryText") ? rep.find("summaryText")->stringValue() : ""},
                {"type", "FrameTelemetryReport"},
                {"variablesReference", 0},
            });
        }
        if (frameId >= static_cast<std::uint32_t>(kFiberFrameIdBase) && fiberScheduler_) {
            const std::uint32_t rel = frameId - static_cast<std::uint32_t>(kFiberFrameIdBase);
            const std::uint32_t fiberId = rel / 8u;
            const std::size_t depthIdx = static_cast<std::size_t>(rel % 8u);
            const auto* fiber = fiberScheduler_->getFiber(fiberId);
            if (fiber &&
                fiber->status != runtime::FiberStatus::Inactive &&
                fiber->status != runtime::FiberStatus::Terminated) {
                const std::uint32_t inspectPc = resolveFiberFramePc(fiberModule_, *fiber, depthIdx);
                const auto* dbg = fiberModule_ ? fiberModule_->debugInfoForPc(inspectPc) : nullptr;
                if (expression == "pc" || expression == "$pc") {
                    return response(request, true, tooling::Json::Object{
                        {"result", std::to_string(fiber->pc)},
                        {"type", "uint32"},
                        {"variablesReference", 0},
                    });
                }
                if (expression == "status" || expression == "$status") {
                    return response(request, true, tooling::Json::Object{
                        {"result", std::string(runtime::fiberStatusDisplayName(fiber->status))},
                        {"type", "FiberStatus"},
                        {"variablesReference", 0},
                    });
                }
                if (expression == "remainingTicks" || expression == "$remainingTicks") {
                    return response(request, true, tooling::Json::Object{
                        {"result", std::to_string(computeFiberRemainingTicks(*fiberScheduler_, *fiber))},
                        {"type", "uint32"},
                        {"variablesReference", 0},
                    });
                }
                if (expression.size() == 2 && expression[0] == 'r' &&
                    expression[1] >= '0' && expression[1] <= '7') {
                    const std::size_t regIdx = static_cast<std::size_t>(expression[1] - '0');
                    return response(request, true, tooling::Json::Object{
                        {"result", std::to_string(fiber->regs[regIdx])},
                        {"type", "int32"},
                        {"variablesReference", 0},
                    });
                }
                if (dbg) {
                    for (std::size_t slot = 0; slot < dbg->registerNames.size(); ++slot) {
                        if (!dbg->registerNames[slot].empty() && dbg->registerNames[slot] == expression) {
                            const std::int32_t rawVal = dbg->registerIsConstFolded[slot]
                                ? dbg->registerConstValues[slot]
                                : fiber->regs[slot];
                            std::string tName;
                            const std::string valStr = formatFiberRegisterValue(
                                rawVal, dbg->registerTypes[slot], dbg->registerTypeIds[slot], tName);
                            return response(request, true, tooling::Json::Object{
                                {"result", valStr},
                                {"type", tName},
                                {"variablesReference", 0},
                            });
                        }
                    }
                }
                return response(request, false, nullptr,
                    "unknown Fiber variable or register: " + expression);
            }
        }
        if (expression == ":heap" || expression == ":heap diff") {
            const bool diffMode = (expression == ":heap diff");
            tooling::Json rep;
            if (session_) {
                session_->runAtSafepoint([&]() {
                    rep = buildHeapSnapshotReport(diffMode);
                });
            } else {
                rep = buildHeapSnapshotReport(diffMode);
            }
            return response(request, true, tooling::Json::Object{
                {"result", rep.find("summaryText") ? rep.find("summaryText")->stringValue() : ""},
                {"type", "HeapSnapshotReport"},
                {"variablesReference", 0},
            });
        }
        if (expression.rfind(":retaining ", 0) == 0) {
            std::string subExpr = expression.substr(11);
            while (!subExpr.empty() && std::isspace(static_cast<unsigned char>(subExpr.front()))) {
                subExpr.erase(subExpr.begin());
            }
            runtime::ObjectRef targetRef;
            std::string report;
            const auto resolveSubExpr = [&]() {
                if (subExpr.size() >= 2 && subExpr[0] == 'o' &&
                    std::isdigit(static_cast<unsigned char>(subExpr[1]))) {
                    const auto slot = static_cast<std::uint32_t>(
                        std::strtoull(subExpr.c_str() + 1, nullptr, 10));
                    if (heap_) {
                        const auto snap = heap_->snapshot();
                        for (const auto& obj : snap.objects) {
                            if (obj.reference.slot == slot) {
                                targetRef = obj.reference;
                                break;
                            }
                        }
                    }
                } else if (session_) {
                    std::string errMsg;
                    if (const auto evalVal = session_->evaluate(subExpr, frameId, &errMsg)) {
                        if (const auto* ref = std::get_if<runtime::ObjectRef>(&*evalVal)) {
                            targetRef = *ref;
                        }
                    }
                }
                if (targetRef.valid()) {
                    report = formatRetainingPathReport(targetRef);
                }
            };
            if (session_) {
                session_->runAtSafepoint(resolveSubExpr);
            } else {
                resolveSubExpr();
            }
            if (!targetRef.valid()) {
                return response(request, false, nullptr,
                    "expression '" + subExpr + "' is not a live managed heap object");
            }
            return response(request, true, tooling::Json::Object{
                {"result", report},
                {"type", "RetainingPath"},
                {"variablesReference", 0},
            });
        }
        std::string errorMessage;
        std::optional<runtime::Value> evaluated;
        std::string evalText;
        std::string evalTypeName;
        std::int64_t evalVarRef = 0;
        const auto runEval = [&]() {
            if (variables_.empty() && session_ && session_->currentStop()) {
                rebuildVariableReferences();
            }
            evaluated = session_->evaluate(expression, frameId, &errorMessage);
            if (evaluated) {
                evalText = runtime::valueToString(*evaluated, heap_.get());
                evalTypeName = describeValueType(*evaluated);
                evalVarRef = registerExpandableValue(*evaluated, frameId);
            }
        };
        if (session_) {
            session_->runAtSafepoint(runEval);
        } else {
            runEval();
        }
        if (!evaluated) {
            return response(request, false, nullptr,
                errorMessage.empty() ? "failed to evaluate expression" : errorMessage);
        }
        return response(request, true, tooling::Json::Object{
            {"result", std::move(evalText)},
            {"type", std::move(evalTypeName)},
            {"variablesReference", evalVarRef},
        });
    }
    if (command == "attach") {
        const auto stopOnEntry = arguments && arguments->find("stopOnEntry")
            ? arguments->find("stopOnEntry")->boolValue(false) : false;
        if (!session_->attach(stopOnEntry)) {
            return response(request, false, nullptr, "cannot attach to debug session");
        }
        return response(request, true, tooling::Json::Object{});
    }
    if (command == "launch") {
        std::string function = arguments && arguments->find("function")
            ? arguments->find("function")->stringValue() : std::string{};
        if (function.empty() && arguments && arguments->find("entry")) {
            function = arguments->find("entry")->stringValue();
        }
        const auto stopOnEntry = arguments && arguments->find("stopOnEntry")
            ? arguments->find("stopOnEntry")->boolValue(false) : false;
        std::vector<runtime::Value> launchArguments;
        if (const auto* values = arguments ? arguments->find("args") : nullptr) {
            for (const auto& value : values->arrayValue()) {
                if (value.isBool()) launchArguments.emplace_back(value.boolValue());
                else if (value.isNumber()) {
                    const auto number = value.numberValue();
                    if (std::isfinite(number) && std::floor(number) == number) {
                        launchArguments.emplace_back(value.integerValue());
                    } else {
                        launchArguments.emplace_back(number);
                    }
                }
                else if (value.isString()) launchArguments.emplace_back(value.stringValue());
                else launchArguments.emplace_back(std::monostate{});
            }
        }
        if (!session_->launch(function, std::move(launchArguments), stopOnEntry)) {
            return response(request, false, nullptr, "debug session is already running or has no program");
        }
        const auto result = response(request, true, tooling::Json::Object{});
        if (events) {
            if (session_->waitForStop(std::chrono::seconds{10})) emitStopped(*events);
            else if (session_->finished()) emitTerminated(*events);
        }
        return result;
    }
    if (command == "configurationDone") return response(request, true, tooling::Json::Object{});
    if (command == "threads") {
        tooling::Json::Array threadList{
            tooling::Json::Object{{"id", kMainThreadId}, {"name", "RealScript main thread"}},
        };
        if (fiberScheduler_ != nullptr) {
            fiberScheduler_->forEachActiveFiber([&](const runtime::FiberContext& fiber) {
                const std::uint32_t inspectPc = fiberModule_
                    ? fiberModule_->effectiveInspectionPc(fiber)
                    : fiber.pc;
                const auto* fnInfo = fiberModule_
                    ? fiberModule_->findFunctionByPc(inspectPc)
                    : nullptr;
                const std::string fnName = fnInfo
                    ? fnInfo->name
                    : ("@pc:" + std::to_string(inspectPc));
                const std::string threadName = "Fiber #" + std::to_string(fiber.fiberId) +
                    " (" + fnName + " - " + runtime::fiberStatusDisplayName(fiber.status) + ")";
                threadList.emplace_back(tooling::Json::Object{
                    {"id", kFiberThreadIdBase + static_cast<std::int64_t>(fiber.fiberId)},
                    {"name", threadName},
                });
            });
        }
        return response(request, true, tooling::Json::Object{
            {"threads", tooling::Json(std::move(threadList))},
        });
    }
    if (command == "stackTrace") {
        const auto threadId = arguments && arguments->find("threadId")
            ? arguments->find("threadId")->integerValue(kMainThreadId)
            : kMainThreadId;
        if (threadId >= kFiberThreadIdBase) {
            tooling::Json::Array fiberFrames;
            const auto fiberId = static_cast<std::uint32_t>(threadId - kFiberThreadIdBase);
            const auto* fiber = fiberScheduler_ ? fiberScheduler_->getFiber(fiberId) : nullptr;
            if (fiber != nullptr &&
                fiber->status != runtime::FiberStatus::Inactive &&
                fiber->status != runtime::FiberStatus::Terminated) {
                const std::size_t totalDepth = 1u + std::min<std::size_t>(
                    fiber->callDepth, fiber->callStack.size());
                for (std::size_t depthIdx = 0; depthIdx < totalDepth; ++depthIdx) {
                    const std::uint32_t inspectPc = resolveFiberFramePc(fiberModule_, *fiber, depthIdx);
                    const auto* fnInfo = fiberModule_ ? fiberModule_->findFunctionByPc(inspectPc) : nullptr;
                    const debug::SourceRange rng = fiberModule_
                        ? fiberModule_->sourceRangeForPc(inspectPc)
                        : debug::SourceRange{};
                    const std::string fnName = fnInfo
                        ? fnInfo->name
                        : ("@pc:" + std::to_string(inspectPc));
                    const std::string rawPath = fnInfo ? fnInfo->debugInfo.sourcePath : std::string{};
                    const std::string resolvedPath =
                        (session_ && session_->controller() && !rawPath.empty())
                            ? session_->controller()->resolveSourcePath(rawPath)
                            : rawPath;
                    const std::int64_t line1 = rng.valid()
                        ? static_cast<std::int64_t>(rng.start.line + 1)
                        : 1;
                    const std::int64_t col1 = rng.valid()
                        ? static_cast<std::int64_t>(rng.start.column + 1)
                        : 1;
                    const std::int64_t frameId = kFiberFrameIdBase +
                        static_cast<std::int64_t>(fiberId) * 8 +
                        static_cast<std::int64_t>(depthIdx);
                    fiberFrames.emplace_back(tooling::Json::Object{
                        {"id", frameId},
                        {"name", fnName},
                        {"line", line1},
                        {"column", col1},
                        {"instructionPointerReference", "pc:" + std::to_string(
                            depthIdx == 0 ? fiber->pc : inspectPc)},
                        {"source", tooling::Json::Object{
                            {"name", rawPath},
                            {"path", resolvedPath},
                        }},
                    });
                }
            }
            const auto total = static_cast<std::int64_t>(fiberFrames.size());
            return response(request, true, tooling::Json::Object{
                {"stackFrames", tooling::Json(std::move(fiberFrames))},
                {"totalFrames", total},
            });
        }

        tooling::Json::Array frames;
        const auto stop = session_->currentStop();
        if (stop) {
            for (const auto& frame : stop->frames) {
                const auto path = session_->controller()
                    ? session_->controller()->resolveSourcePath(frame.sourcePath)
                    : frame.sourcePath;
                frames.emplace_back(tooling::Json::Object{
                    {"id", static_cast<std::int64_t>(frame.id)},
                    {"name", frame.function},
                    {"line", static_cast<std::int64_t>(frame.location.start.line + 1)},
                    {"column", static_cast<std::int64_t>(frame.location.start.column + 1)},
                    {"source", tooling::Json::Object{{"name", frame.sourcePath}, {"path", path}}},
                });
            }
        }
        return response(request, true, tooling::Json::Object{
            {"stackFrames", tooling::Json(std::move(frames))},
            {"totalFrames", stop ? static_cast<std::int64_t>(stop->frames.size()) : 0},
        });
    }
    if (command == "scopes") {
        const auto frameId = arguments && arguments->find("frameId")
            ? std::max<std::int64_t>(1, arguments->find("frameId")->integerValue(1)) : 1;
        if (frameId >= kFiberFrameIdBase) {
            return response(request, true, tooling::Json::Object{{"scopes", tooling::Json::Array{
                tooling::Json::Object{{"name", "Fiber State"}, {"variablesReference", frameId * 2}, {"expensive", false}},
                tooling::Json::Object{{"name", "Locals"}, {"variablesReference", frameId * 2 + 1}, {"expensive", false}},
            }}});
        }
        return response(request, true, tooling::Json::Object{{"scopes", tooling::Json::Array{
            tooling::Json::Object{{"name", "Arguments"}, {"variablesReference", frameId * 2}, {"expensive", false}},
            tooling::Json::Object{{"name", "Locals"}, {"variablesReference", frameId * 2 + 1}, {"expensive", false}},
        }}});
    }
    if (command == "variables") {
        const auto reference = arguments && arguments->find("variablesReference")
            ? arguments->find("variablesReference")->integerValue() : 0;
        if (reference >= kFiberFrameIdBase * 2 && reference < kFirstChildVariablesReference) {
            tooling::Json::Array fiberVars;
            const std::int64_t frameId = reference / 2;
            const bool isLocalsScope = (reference % 2) != 0;
            const std::int64_t rel = frameId - kFiberFrameIdBase;
            const auto fiberId = static_cast<std::uint32_t>(rel / 8);
            const auto depthIdx = static_cast<std::size_t>(rel % 8);
            const auto* fiber = fiberScheduler_ ? fiberScheduler_->getFiber(fiberId) : nullptr;
            if (fiber != nullptr &&
                fiber->status != runtime::FiberStatus::Inactive &&
                fiber->status != runtime::FiberStatus::Terminated) {
                const std::uint32_t inspectPc = resolveFiberFramePc(fiberModule_, *fiber, depthIdx);
                const auto* dbg = fiberModule_ ? fiberModule_->debugInfoForPc(inspectPc) : nullptr;
                const std::uint32_t remTicks = computeFiberRemainingTicks(*fiberScheduler_, *fiber);
                if (!isLocalsScope) {
                    fiberVars.emplace_back(tooling::Json::Object{
                        {"name", "pc"},
                        {"value", std::to_string(fiber->pc)},
                        {"type", "uint32"},
                        {"variablesReference", 0},
                    });
                    fiberVars.emplace_back(tooling::Json::Object{
                        {"name", "status"},
                        {"value", std::string(runtime::fiberStatusDisplayName(fiber->status))},
                        {"type", "FiberStatus"},
                        {"variablesReference", 0},
                    });
                    fiberVars.emplace_back(tooling::Json::Object{
                        {"name", "remainingTicks"},
                        {"value", std::to_string(remTicks)},
                        {"type", "uint32"},
                        {"variablesReference", 0},
                    });
                    fiberVars.emplace_back(tooling::Json::Object{
                        {"name", "wakeTick"},
                        {"value", std::to_string(fiber->wakeTick)},
                        {"type", "uint32"},
                        {"variablesReference", 0},
                    });
                    fiberVars.emplace_back(tooling::Json::Object{
                        {"name", "callDepth"},
                        {"value", std::to_string(fiber->callDepth)},
                        {"type", "uint8"},
                        {"variablesReference", 0},
                    });
                    fiberVars.emplace_back(tooling::Json::Object{
                        {"name", "unbreakable"},
                        {"value", fiber->unbreakableSection ? "true" : "false"},
                        {"type", "bool"},
                        {"variablesReference", 0},
                    });
                    fiberVars.emplace_back(tooling::Json::Object{
                        {"name", "boundEntity"},
                        {"value", fiber->boundEntity.valid()
                            ? (std::to_string(fiber->boundEntity.slot) + ":" +
                               std::to_string(fiber->boundEntity.generation))
                            : std::string("none")},
                        {"type", "GenerationalEntityHandle"},
                        {"variablesReference", 0},
                    });
                    if (fiber->status == runtime::FiberStatus::WaitingPredicate) {
                        fiberVars.emplace_back(tooling::Json::Object{
                            {"name", "waitPredicateChannel"},
                            {"value", std::to_string(fiber->waitPredicateChannel)},
                            {"type", "uint8"},
                            {"variablesReference", 0},
                        });
                        fiberVars.emplace_back(tooling::Json::Object{
                            {"name", "waitTargetKey"},
                            {"value", std::to_string(fiber->waitTargetKey)},
                            {"type", "uint16"},
                            {"variablesReference", 0},
                        });
                        fiberVars.emplace_back(tooling::Json::Object{
                            {"name", "waitRequiredValue"},
                            {"value", std::to_string(fiber->waitRequiredValue)},
                            {"type", "uint32"},
                            {"variablesReference", 0},
                        });
                    }
                } else {
                    if (dbg != nullptr) {
                        for (std::size_t slot = 0; slot < dbg->registerNames.size(); ++slot) {
                            if (dbg->registerNames[slot].empty()) continue;
                            const std::int32_t rawVal = dbg->registerIsConstFolded[slot]
                                ? dbg->registerConstValues[slot]
                                : fiber->regs[slot];
                            std::string tName;
                            const std::string valStr = formatFiberRegisterValue(
                                rawVal, dbg->registerTypes[slot], dbg->registerTypeIds[slot], tName);
                            fiberVars.emplace_back(tooling::Json::Object{
                                {"name", dbg->registerNames[slot]},
                                {"value", valStr},
                                {"type", tName},
                                {"variablesReference", 0},
                            });
                        }
                    }
                    for (std::size_t slot = 0; slot < fiber->regs.size(); ++slot) {
                        fiberVars.emplace_back(tooling::Json::Object{
                            {"name", "r" + std::to_string(slot)},
                            {"value", std::to_string(fiber->regs[slot])},
                            {"type", "int32"},
                            {"variablesReference", 0},
                        });
                    }
                }
            }
            return response(request, true, tooling::Json::Object{
                {"variables", tooling::Json(std::move(fiberVars))},
            });
        }

        tooling::Json::Array values;
        const auto collectVariables = [&]() {
            if (variables_.empty() && session_ && session_->currentStop()) {
                rebuildVariableReferences();
            }
            const auto found = variables_.find(reference);
            if (found != variables_.end()) {
                std::uint32_t ownerFrameId = 1u;
                if (const auto fIt = variableFrameIds_.find(reference);
                    fIt != variableFrameIds_.end()) {
                    ownerFrameId = fIt->second;
                } else if (reference >= 2 && reference < kFirstChildVariablesReference) {
                    ownerFrameId = static_cast<std::uint32_t>(reference / 2);
                }
                const auto listCopy = found->second;
                for (const auto& variable : listCopy) {
                    const auto childRef = registerExpandableValue(variable.value, ownerFrameId);
                    values.emplace_back(tooling::Json::Object{
                        {"name", variable.name},
                        {"value", runtime::valueToString(variable.value, heap_.get())},
                        {"type", describeValueType(variable.value, variable.type)},
                        {"variablesReference", childRef},
                    });
                }
            }
        };
        if (session_) {
            session_->runAtSafepoint(collectVariables);
        } else {
            collectVariables();
        }
        return response(request, true, tooling::Json::Object{{"variables", tooling::Json(std::move(values))}});
    }
    if (command == "setVariable") {
        const auto reference = arguments && arguments->find("variablesReference")
            ? arguments->find("variablesReference")->integerValue() : 2;
        const auto name = arguments && arguments->find("name")
            ? arguments->find("name")->stringValue() : std::string{};
        const auto valExpr = arguments && arguments->find("value")
            ? arguments->find("value")->stringValue() : std::string{};
        tooling::Json result;
        const auto doSetVariable = [&]() -> tooling::Json {
        if (reference >= kFirstChildVariablesReference) {
            const auto ownerIt = childOwnerObjects_.find(reference);
            if (ownerIt == childOwnerObjects_.end() || !heap_ || !heap_->isAlive(ownerIt->second)) {
                return response(request, false, nullptr,
                    "cannot mutate non-heap child variable reference");
            }
            const auto ownerRef = ownerIt->second;
            const std::uint32_t evalFrameId = [&]() -> std::uint32_t {
                const auto fIt = variableFrameIds_.find(reference);
                return fIt != variableFrameIds_.end() ? fIt->second : 1u;
            }();
            const auto parseBracketIndex = [](const std::string& label, std::size_t& outIdx) noexcept -> bool {
                if (label.size() < 3 || label.front() != '[' || label.back() != ']') return false;
                std::size_t acc = 0;
                for (std::size_t pos = 1; pos + 1 < label.size(); ++pos) {
                    const char ch = label[pos];
                    if (ch < '0' || ch > '9') return false;
                    const std::size_t digit = static_cast<std::size_t>(ch - '0');
                    if (acc > (std::numeric_limits<std::size_t>::max() - digit) / 10) return false;
                    acc = acc * 10 + digit;
                }
                outIdx = acc;
                return true;
            };
            if (ownerRef.kind != runtime::ObjectKind::Array) {
                const auto preTypeId = heap_->objectTypeId(ownerRef).value_or(0);
                const auto* preTypeDesc = (program_ && preTypeId != 0) ? program_->resolveType(preTypeId) : nullptr;
                if (preTypeDesc && preTypeDesc->name.rfind("HashSet__", 0) == 0) {
                    return response(request, false, nullptr,
                        "cannot modify HashSet elements in-place without rehashing; use Remove and Add instead");
                }
            }
            std::string errMsg;
            const auto evaluated = session_->evaluate(valExpr, evalFrameId, &errMsg);
            if (!evaluated) {
                return response(request, false, nullptr,
                    errMsg.empty() ? "failed to evaluate new value" : errMsg);
            }
            runtime::RuntimeError rtErr;
            if (ownerRef.kind == runtime::ObjectKind::Array) {
                std::size_t idx = 0;
                if (!parseBracketIndex(name, idx)) {
                    return response(request, false, nullptr, "invalid array element name: " + name);
                }
                if (!heap_->arraySet(ownerRef, idx, *evaluated, &rtErr)) {
                    return response(request, false, nullptr, rtErr.message);
                }
            } else {
                const auto typeId = heap_->objectTypeId(ownerRef).value_or(0);
                const auto* typeDesc = (program_ && typeId != 0) ? program_->resolveType(typeId) : nullptr;
                const auto fCount = heap_->fieldCount(ownerRef).value_or(0);
                auto getOwnerField = [&](std::string_view fname, std::size_t fallbackIdx) -> std::optional<runtime::Value> {
                    if (typeDesc) {
                        for (std::size_t i = 0; i < typeDesc->fields.size(); ++i) {
                            const auto& fld = typeDesc->fields[i];
                            if (fld.staticField || fld.constField) continue;
                            if (fld.name == fname) {
                                const std::size_t slotIdx = fld.index < fCount ? fld.index : i;
                                return heap_->fieldGet(ownerRef, slotIdx);
                            }
                        }
                    }
                    return fallbackIdx < fCount ? heap_->fieldGet(ownerRef, fallbackIdx) : std::nullopt;
                };
                if (typeDesc && typeDesc->name.rfind("HashSet__", 0) == 0) {
                    return response(request, false, nullptr,
                        "cannot mutate HashSet element in-place without rehashing: " + name);
                } else if (typeDesc && typeDesc->name.rfind("Dictionary__", 0) == 0 && fCount >= 5) {
                    if (name.size() < 2 || name.front() != '[' || name.back() != ']') {
                        return response(request, false, nullptr,
                            "cannot modify read-only Dictionary property: " + name);
                    }
                    const auto keysVal = getOwnerField("keys", 0);
                    const auto valsVal = getOwnerField("values", 1);
                    const auto countVal = getOwnerField("count", 4);
                    if (!keysVal || !valsVal || !countVal ||
                        !std::holds_alternative<std::int64_t>(*countVal)) {
                        return response(request, false, nullptr, "corrupted Dictionary state");
                    }
                    const auto logicalCount = static_cast<std::size_t>(
                        std::max<std::int64_t>(0, std::get<std::int64_t>(*countVal)));
                    const auto* kArr = std::get_if<runtime::ObjectRef>(&*keysVal);
                    const auto* vArr = std::get_if<runtime::ObjectRef>(&*valsVal);
                    if (!kArr || !vArr || !kArr->valid() || !vArr->valid() ||
                        !heap_->isAlive(*kArr) || !heap_->isAlive(*vArr)) {
                        return response(request, false, nullptr, "Dictionary backing storage is not alive");
                    }
                    std::size_t matchedIdx = std::numeric_limits<std::size_t>::max();
                    for (std::size_t i = 0; i < logicalCount; ++i) {
                        const auto kElem = heap_->arrayGet(*kArr, i);
                        if (!kElem) continue;
                        const std::string keyText = runtime::valueToString(*kElem, heap_.get());
                        if (("[" + keyText + "]") == name || ("[\"" + keyText + "\"]") == name) {
                            matchedIdx = i;
                            break;
                        }
                    }
                    if (matchedIdx == std::numeric_limits<std::size_t>::max()) {
                        return response(request, false, nullptr, "Dictionary key not found: " + name);
                    }
                    if (!heap_->arraySet(*vArr, matchedIdx, *evaluated, &rtErr)) {
                        return response(request, false, nullptr, rtErr.message);
                    }
                } else if (typeDesc && typeDesc->name.rfind("List__", 0) == 0 && fCount >= 2) {
                    std::size_t idx = 0;
                    if (!parseBracketIndex(name, idx)) {
                        return response(request, false, nullptr,
                            "cannot modify List property '" + name + "'; specify an [index] element");
                    }
                    const auto itemsVal = getOwnerField("items", 0);
                    const auto countVal = getOwnerField("count", 1);
                    const auto logicalCount = (countVal && std::holds_alternative<std::int64_t>(*countVal))
                        ? static_cast<std::size_t>(std::max<std::int64_t>(0, std::get<std::int64_t>(*countVal)))
                        : 0;
                    const auto* arrRef = itemsVal ? std::get_if<runtime::ObjectRef>(&*itemsVal) : nullptr;
                    if (!arrRef || !arrRef->valid() || !heap_->isAlive(*arrRef) || idx >= logicalCount) {
                        return response(request, false, nullptr, "List index out of range: " + name);
                    }
                    if (!heap_->arraySet(*arrRef, idx, *evaluated, &rtErr)) {
                        return response(request, false, nullptr, rtErr.message);
                    }
                } else if (typeDesc && typeDesc->name.rfind("Stack__", 0) == 0 && fCount >= 2) {
                    std::size_t idx = 0;
                    if (!parseBracketIndex(name, idx)) {
                        return response(request, false, nullptr,
                            "cannot modify Stack property '" + name + "'; specify an [index] element");
                    }
                    const auto itemsVal = getOwnerField("items", 0);
                    const auto countVal = getOwnerField("count", 1);
                    const auto logicalCount = (countVal && std::holds_alternative<std::int64_t>(*countVal))
                        ? static_cast<std::size_t>(std::max<std::int64_t>(0, std::get<std::int64_t>(*countVal)))
                        : 0;
                    const auto* arrRef = itemsVal ? std::get_if<runtime::ObjectRef>(&*itemsVal) : nullptr;
                    if (!arrRef || !arrRef->valid() || !heap_->isAlive(*arrRef) || idx >= logicalCount) {
                        return response(request, false, nullptr, "Stack index out of range: " + name);
                    }
                    if (!heap_->arraySet(*arrRef, logicalCount - 1 - idx, *evaluated, &rtErr)) {
                        return response(request, false, nullptr, rtErr.message);
                    }
                } else if (typeDesc && typeDesc->name.rfind("Queue__", 0) == 0 && fCount >= 4) {
                    std::size_t idx = 0;
                    if (!parseBracketIndex(name, idx)) {
                        return response(request, false, nullptr,
                            "cannot modify Queue property '" + name + "'; specify an [index] element");
                    }
                    const auto itemsVal = getOwnerField("items", 0);
                    const auto headVal = getOwnerField("head", 1);
                    const auto countVal = getOwnerField("count", 3);
                    const auto logicalCount = (countVal && std::holds_alternative<std::int64_t>(*countVal))
                        ? static_cast<std::size_t>(std::max<std::int64_t>(0, std::get<std::int64_t>(*countVal)))
                        : 0;
                    const auto headIdx = (headVal && std::holds_alternative<std::int64_t>(*headVal))
                        ? static_cast<std::size_t>(std::max<std::int64_t>(0, std::get<std::int64_t>(*headVal)))
                        : 0;
                    const auto* arrRef = itemsVal ? std::get_if<runtime::ObjectRef>(&*itemsVal) : nullptr;
                    const auto arrLen = (arrRef && arrRef->valid() && heap_->isAlive(*arrRef))
                        ? heap_->arrayLength(*arrRef).value_or(0)
                        : 0;
                    if (!arrRef || arrLen == 0 || idx >= logicalCount) {
                        return response(request, false, nullptr, "Queue index out of range: " + name);
                    }
                    if (!heap_->arraySet(*arrRef, (headIdx + idx) % arrLen, *evaluated, &rtErr)) {
                        return response(request, false, nullptr, rtErr.message);
                    }
                } else {
                    std::size_t targetIdx = std::numeric_limits<std::size_t>::max();
                    if (typeDesc) {
                        for (std::size_t i = 0; i < typeDesc->fields.size(); ++i) {
                            const auto& fld = typeDesc->fields[i];
                            if (fld.staticField || fld.constField) continue;
                            if (fld.name == name) {
                                targetIdx = fld.index < fCount ? fld.index : i;
                                break;
                            }
                        }
                    }
                    if (targetIdx == std::numeric_limits<std::size_t>::max() ||
                        !heap_->fieldSet(ownerRef, targetIdx, *evaluated, &rtErr)) {
                        return response(request, false, nullptr,
                            rtErr.message.empty() ? ("unknown field: " + name) : rtErr.message);
                    }
                }
            }
            if (auto it = variables_.find(reference); it != variables_.end()) {
                for (auto& childVar : it->second) {
                    if (childVar.name == name) {
                        childVar.value = *evaluated;
                        break;
                    }
                }
            }
            if (const auto stop = session_->currentStop()) {
                for (const auto& frame : stop->frames) {
                    variables_[static_cast<std::int64_t>(frame.id) * 2] = frame.arguments;
                    variables_[static_cast<std::int64_t>(frame.id) * 2 + 1] = frame.locals;
                }
            }
            const auto childRef = registerExpandableValue(*evaluated, evalFrameId);
            return response(request, true, tooling::Json::Object{
                {"value", runtime::valueToString(*evaluated, heap_.get())},
                {"type", describeValueType(*evaluated)},
                {"variablesReference", childRef},
            });
        }
        const auto frameId = [&]() -> std::uint32_t {
            const auto fIt = variableFrameIds_.find(reference);
            if (fIt != variableFrameIds_.end()) return fIt->second;
            return static_cast<std::uint32_t>(std::max<std::int64_t>(1, reference / 2));
        }();
        runtime::Value updatedVal{};
        std::string errMsg;
        if (!session_->setVariable(name, valExpr, frameId, &updatedVal, &errMsg)) {
            return response(request, false, nullptr,
                errMsg.empty() ? "failed to set variable" : errMsg);
        }
        rebuildVariableReferences();
        const auto childRef = registerExpandableValue(updatedVal, frameId);
        return response(request, true, tooling::Json::Object{
            {"value", runtime::valueToString(updatedVal, heap_.get())},
            {"type", describeValueType(updatedVal)},
            {"variablesReference", childRef},
        });
        };
        if (session_) {
            session_->runAtSafepoint([&]() {
                result = doSetVariable();
            });
        } else {
            result = doSetVariable();
        }
        return result;
    }
    if (command == "continue" || command == "next" ||
        command == "stepIn" || command == "stepOut") {
        variables_.clear();
        childOwnerObjects_.clear();
        variableFrameIds_.clear();
        nextChildRef_ = kFirstChildVariablesReference;
        session_->resume(commandMode(command));
        const auto result = response(request, true, tooling::Json::Object{{"allThreadsContinued", true}});
        if (events) {
            if (session_->waitForStop(std::chrono::seconds{10})) emitStopped(*events);
            else if (session_->finished()) emitTerminated(*events);
        }
        return result;
    }
    if (command == "pause") {
        session_->pause();
        const auto result = response(request, true, tooling::Json::Object{});
        if (events && session_->waitForStop(std::chrono::seconds{10})) {
            emitStopped(*events);
        }
        return result;
    }
    if (command == "disconnect" || command == "terminate") {
        variables_.clear();
        childOwnerObjects_.clear();
        variableFrameIds_.clear();
        nextChildRef_ = kFirstChildVariablesReference;
        if (session_->isAttached()) {
            session_->detach();
        } else {
            session_->terminate();
            session_->join();
        }
        if (events) emitTerminated(*events);
        return response(request, true, tooling::Json::Object{});
    }
    return response(request, false, nullptr, "unsupported DAP command: " + command);
}

int DapServer::run(std::istream& input, std::ostream& output) {
    std::mutex outputMutex;
    std::thread monitor;
    std::atomic<bool> suppressMonitor{false};
    std::atomic<bool> monitorActive{false};

    const auto joinMonitor = [&] {
        if (monitor.joinable()) monitor.join();
        monitorActive.store(false, std::memory_order_release);
    };
    const auto startMonitor = [&] {
        if (monitorActive.load(std::memory_order_acquire)) return;
        joinMonitor();
        suppressMonitor.store(false);
        monitorActive.store(true, std::memory_order_release);
        monitor = std::thread([&] {
            const auto stopped = session_->waitForStop();
            if (!suppressMonitor.load()) {
                if (!stopped) session_->join();
                std::lock_guard<std::mutex> lock(outputMutex);
                if (stopped) emitStopped(output);
                else emitTerminated(output);
            }
            monitorActive.store(false, std::memory_order_release);
        });
    };

    std::string body;
    while (tooling::readProtocolMessage(input, body)) {
        std::string error;
        auto request = tooling::Json::parse(body, error);
        if (!request) continue;
        const auto* commandValue = request->find("command");
        const auto command = commandValue ? commandValue->stringValue() : std::string{};
        const auto asynchronous = command == "launch" || command == "attach" ||
            command == "continue" || command == "next" ||
            command == "stepIn" || command == "stepOut" || command == "pause";
        const auto terminal = command == "disconnect" || command == "terminate";

        if (terminal) suppressMonitor.store(true);
        if (asynchronous && !monitorActive.load(std::memory_order_acquire)) {
            joinMonitor();
        }
        std::ostringstream events;
        const auto responseValue = handle(*request, asynchronous ? nullptr : &events);
        if (terminal) joinMonitor();
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            tooling::writeProtocolMessage(output, responseValue);
            output << events.str();
            if (terminal) emitTerminated(output);
            output.flush();
        }
        const auto succeeded = responseValue.find("success") &&
            responseValue.find("success")->boolValue();
        if (asynchronous && succeeded) startMonitor();
        if (terminal) break;
    }

    if (monitor.joinable()) {
        suppressMonitor.store(true);
        if (session_->isAttached()) {
            session_->detach();
        } else {
            session_->terminate();
            session_->join();
        }
        joinMonitor();
    }
    return 0;
}

int DapServer::serveTcp(
    std::uint16_t port,
    const std::string& bindAddress,
    std::uint16_t* outBoundPort) {
    SocketHandle listenSock = createListeningTcpSocket(port, bindAddress, outBoundPort);
    if (listenSock == kInvalidSocket) return 1;

    sockaddr_in clientAddr{};
#if defined(_WIN32)
    int clientLen = sizeof(clientAddr);
#else
    socklen_t clientLen = sizeof(clientAddr);
#endif
    SocketHandle clientSock = ::accept(
        listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    closeSocketHandle(listenSock);
    if (clientSock == kInvalidSocket) return 1;

    int noDelay = 1;
#if defined(_WIN32)
    (void)::setsockopt(
        clientSock, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
#else
    (void)::setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
#endif

    SocketStreamBuf streamBuf(clientSock);
    std::iostream socketStream(&streamBuf);
    const int rc = run(socketStream, socketStream);
    closeSocketHandle(clientSock);
    return rc;
}

EmbeddedDapServer::EmbeddedDapServer(
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<runtime::ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      session_(std::make_shared<DebugSession>(program_, heap_)) {}

EmbeddedDapServer::~EmbeddedDapServer() {
    stop();
}

bool EmbeddedDapServer::start(
    std::uint16_t port,
    const std::string& bindAddress) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load(std::memory_order_acquire)) return true;

    std::uint16_t actualPort = 0;
    SocketHandle listenSock = createListeningTcpSocket(port, bindAddress, &actualPort);
    if (listenSock == kInvalidSocket) return false;

    listenSocket_ = static_cast<std::intptr_t>(listenSock);
    boundPort_.store(actualPort, std::memory_order_release);
    stopRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    acceptThread_ = std::thread([this, listenSock]() {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            sockaddr_in clientAddr{};
#if defined(_WIN32)
            int clientLen = sizeof(clientAddr);
#else
            socklen_t clientLen = sizeof(clientAddr);
#endif
            SocketHandle clientSock = ::accept(
                listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientSock == kInvalidSocket) {
                break;
            }
            if (stopRequested_.load(std::memory_order_acquire)) {
                closeSocketHandle(clientSock);
                break;
            }
            int noDelay = 1;
#if defined(_WIN32)
            (void)::setsockopt(
                clientSock, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
#else
            (void)::setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
#endif
            {
                std::lock_guard<std::mutex> clientLock(mutex_);
                activeClientSocket_ = static_cast<std::intptr_t>(clientSock);
            }

            std::shared_ptr<const runtime::ProgramImage> currentProg;
            {
                std::lock_guard<std::mutex> progLock(mutex_);
                currentProg = program_;
            }
            DapServer server(session_, currentProg, heap_);
            SocketStreamBuf streamBuf(clientSock);
            std::iostream socketStream(&streamBuf);
            (void)server.run(socketStream, socketStream);

            {
                std::lock_guard<std::mutex> clientLock(mutex_);
                if (activeClientSocket_ == static_cast<std::intptr_t>(clientSock)) {
                    closeSocketHandle(clientSock);
                    activeClientSocket_ = -1;
                }
            }
        }
        running_.store(false, std::memory_order_release);
    });
    return true;
}

void EmbeddedDapServer::stop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (listenSocket_ != -1) {
            closeSocketHandle(static_cast<SocketHandle>(listenSocket_));
            listenSocket_ = -1;
        }
        if (activeClientSocket_ != -1) {
            closeSocketHandle(static_cast<SocketHandle>(activeClientSocket_));
            activeClientSocket_ = -1;
        }
    }
    if (session_) {
        if (session_->isAttached()) {
            session_->detach();
        } else {
            session_->terminate();
        }
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    running_.store(false, std::memory_order_release);
    boundPort_.store(0, std::memory_order_release);
}

void EmbeddedDapServer::rebindProgram(std::shared_ptr<const runtime::ProgramImage> program) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        program_ = program;
    }
    if (session_) {
        session_->rebindProgram(std::move(program));
    }
}

} // namespace realscript::debug
