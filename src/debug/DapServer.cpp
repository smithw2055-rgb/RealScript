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

std::int64_t DapServer::registerExpandableValue(const runtime::Value& value) {
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
            return refId;
        }
        const auto typeId = heap_->objectTypeId(*objRef).value_or(0);
        const auto* typeDesc = (program_ && typeId != 0) ? program_->resolveType(typeId) : nullptr;
        const auto fCount = heap_->fieldCount(*objRef).value_or(0);
        std::vector<DebugVariable> children;
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
        return refId;
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
        return refId;
    }
    return 0;
}

void DapServer::rebuildVariableReferences() {
    variables_.clear();
    childOwnerObjects_.clear();
    nextChildRef_ = 1000;
    const auto stop = session_->currentStop();
    if (!stop) return;
    for (const auto& frame : stop->frames) {
        variables_[static_cast<std::int64_t>(frame.id) * 2] = frame.arguments;
        variables_[static_cast<std::int64_t>(frame.id) * 2 + 1] = frame.locals;
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
                if (const auto* st = std::get_if<runtime::StructValue>(&val);
                    st && st->storage && visited.insert(st->storage.get()).second) {
                    for (const auto& f : st->storage->fields) {
                        if (self(self, f, target, visited)) return true;
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
        const auto runEval = [&]() {
            evaluated = session_->evaluate(expression, frameId, &errorMessage);
            if (evaluated) {
                evalText = runtime::valueToString(*evaluated, heap_.get());
                evalTypeName = semantic::primitiveTypeName(runtime::valueType(*evaluated));
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
            {"variablesReference", 0},
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
        return response(request, true, tooling::Json::Object{{"threads", tooling::Json::Array{
            tooling::Json::Object{{"id", 1}, {"name", "RealScript main thread"}},
        }}});
    }
    if (command == "stackTrace") {
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
            ? arguments->find("frameId")->integerValue() : 0;
        return response(request, true, tooling::Json::Object{{"scopes", tooling::Json::Array{
            tooling::Json::Object{{"name", "Arguments"}, {"variablesReference", frameId * 2}, {"expensive", false}},
            tooling::Json::Object{{"name", "Locals"}, {"variablesReference", frameId * 2 + 1}, {"expensive", false}},
        }}});
    }
    if (command == "variables") {
        const auto reference = arguments && arguments->find("variablesReference")
            ? arguments->find("variablesReference")->integerValue() : 0;
        tooling::Json::Array values;
        const auto collectVariables = [&]() {
            const auto found = variables_.find(reference);
            if (found != variables_.end()) {
                const auto listCopy = found->second;
                for (const auto& variable : listCopy) {
                    const auto childRef = registerExpandableValue(variable.value);
                    values.emplace_back(tooling::Json::Object{
                        {"name", variable.name},
                        {"value", runtime::valueToString(variable.value, heap_.get())},
                        {"type", semantic::primitiveTypeName(variable.type)},
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
        if (reference >= 1000) {
            const auto ownerIt = childOwnerObjects_.find(reference);
            if (ownerIt == childOwnerObjects_.end() || !heap_ || !heap_->isAlive(ownerIt->second)) {
                return response(request, false, nullptr,
                    "cannot mutate non-heap child variable reference");
            }
            std::string errMsg;
            const auto evaluated = session_->evaluate(valExpr, 1u, &errMsg);
            if (!evaluated) {
                return response(request, false, nullptr,
                    errMsg.empty() ? "failed to evaluate new value" : errMsg);
            }
            const auto ownerRef = ownerIt->second;
            runtime::RuntimeError rtErr;
            if (ownerRef.kind == runtime::ObjectKind::Array) {
                if (name.size() < 3 || name.front() != '[' || name.back() != ']') {
                    return response(request, false, nullptr, "invalid array element name: " + name);
                }
                const auto idx = static_cast<std::size_t>(
                    std::strtoull(name.substr(1, name.size() - 2).c_str(), nullptr, 10));
                if (!heap_->arraySet(ownerRef, idx, *evaluated, &rtErr)) {
                    return response(request, false, nullptr, rtErr.message);
                }
            } else {
                const auto typeId = heap_->objectTypeId(ownerRef).value_or(0);
                const auto* typeDesc = (program_ && typeId != 0) ? program_->resolveType(typeId) : nullptr;
                std::size_t targetIdx = std::numeric_limits<std::size_t>::max();
                if (typeDesc) {
                    for (std::size_t i = 0; i < typeDesc->fields.size(); ++i) {
                        const auto& fld = typeDesc->fields[i];
                        if (fld.staticField || fld.constField) continue;
                        if (fld.name == name) {
                            targetIdx = i;
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
            const auto childRef = registerExpandableValue(*evaluated);
            return response(request, true, tooling::Json::Object{
                {"value", runtime::valueToString(*evaluated, heap_.get())},
                {"type", semantic::primitiveTypeName(runtime::valueType(*evaluated))},
                {"variablesReference", childRef},
            });
        }
        const auto frameId = static_cast<std::uint32_t>(std::max<std::int64_t>(1, reference / 2));
        runtime::Value updatedVal{};
        std::string errMsg;
        if (!session_->setVariable(name, valExpr, frameId, &updatedVal, &errMsg)) {
            return response(request, false, nullptr,
                errMsg.empty() ? "failed to set variable" : errMsg);
        }
        rebuildVariableReferences();
        const auto childRef = registerExpandableValue(updatedVal);
        return response(request, true, tooling::Json::Object{
            {"value", runtime::valueToString(updatedVal, heap_.get())},
            {"type", semantic::primitiveTypeName(runtime::valueType(updatedVal))},
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
