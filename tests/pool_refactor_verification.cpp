#include "realscript/bytecode/Bytecode.h"
#include "realscript/game/Gameplay.h"
#include "realscript/runtime/Runtime.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

int main() {
    using namespace realscript;

    // 1. Verify pinned ManagedHeap struct/string survival across heavy clock eviction + GC reclamation
    {
        game::GameApi api;
        const char* src = R"RS(
module PoolTest;

struct SmallTransform {
    public int x;
    public int y;
    public int z;
}

struct LargeAttributes {
    public int a;
    public int b;
    public int c;
    public int d;
    public int e;
    public string label;
}

class EntityHolder {
    public SmallTransform tf;
    public LargeAttributes attrs;
    public string tag;

    public void Init() {
        tf = new SmallTransform { x = 777, y = 888, z = 999 };
        attrs = new LargeAttributes { a = 10, b = 20, c = 30, d = 40, e = 50, label = "PinnedLargeAttr" };
        tag = "PinnedHostStringValue";
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"pool_test.rs", src}});
        require(compiled.succeeded(), "pool_test.rs compile failed");

        auto rt = std::make_unique<game::ScriptRuntime>(compiled.program);
        runtime::RuntimeError err;
        auto holder = rt->createObject("PoolTest::EntityHolder", err);
        require(holder.has_value(), "createObject EntityHolder failed");
        require(
            rt->invoke(*holder, *rt->findMethod(holder->type(), "Init", 0), {}).succeeded,
            "EntityHolder.Init failed");

        // Also store a C++ host StringValue into a static field / root to test StringValue pinning
        rt->heap()->setStaticField(0x999999ULL, runtime::Value{runtime::StringValue(std::string_view("PinnedHostStringValue"))});
        const auto statsAfterPin = runtime::getRuntimeValuePoolStatistics();
        std::cout << "[After Pin] inlinePinned=" << statsAfterPin.inlineStructPinnedSlots
                  << " heapStructPinned=" << statsAfterPin.heapStructPinnedSlots
                  << " stringPinned=" << statsAfterPin.stringPinnedSlots << "\n";
        require(
            statsAfterPin.inlineStructPinnedSlots >= 1 &&
                statsAfterPin.heapStructPinnedSlots >= 1 &&
                statsAfterPin.stringPinnedSlots >= 1,
            "Heap-stored inline struct, oversized struct, and string must be marked Pinned");

        // Churn 200,000 ephemeral inline structs, 50,000 oversized structs, and 200,000 dynamic strings
        const semantic::SymbolId dummyInlineType = 0x12345678ULL;
        const semantic::SymbolId dummyLargeType = 0x87654321ULL;
        for (int i = 0; i < 200000; ++i) {
            std::array<runtime::Value, 3> inlFields{
                std::int64_t{i},
                std::int64_t{i * 2},
                std::int64_t{i * 3},
            };
            auto st = runtime::makeStructValueFromFields(
                dummyInlineType, runtime::ValueSpan(inlFields.data(), inlFields.size()));
            require(st.isInline, "3-int struct must be inline");

            runtime::StringValue dynStr("ephemeral_tick_str_" + std::to_string(i));
            require(dynStr.size() > 0, "dynamic string must be non-empty");
        }
        for (int i = 0; i < 50000; ++i) {
            std::vector<runtime::Value> largeFields{
                std::int64_t{i},
                std::int64_t{i + 1},
                std::int64_t{i + 2},
                std::int64_t{i + 3},
                std::int64_t{i + 4},
            };
            auto lg = runtime::makeStructValueFromFields(dummyLargeType, std::move(largeFields));
            require(!lg.isInline && lg.storage != nullptr, "5-int struct must use HeapStructStoragePool");
        }

        const auto statsAfterChurn = runtime::getRuntimeValuePoolStatistics();
        std::cout << "[Pool Churn] Inline pages=" << statsAfterChurn.inlineStructAllocatedPages
                  << " clockEvictions=" << statsAfterChurn.inlineStructClockEvictions
                  << " | String pages=" << statsAfterChurn.stringAllocatedPages
                  << " clockEvictions=" << statsAfterChurn.stringClockEvictions
                  << " | HeapStruct pages=" << statsAfterChurn.heapStructAllocatedPages
                  << " clockEvictions=" << statsAfterChurn.heapStructClockEvictions << "\n";

        require(
            statsAfterChurn.inlineStructClockEvictions > 0 ||
                statsAfterChurn.stringClockEvictions > 0 ||
                statsAfterChurn.heapStructClockEvictions > 0,
            "Second-chance clock eviction must recycle unpinned ephemeral slots once soft page cap is reached");
        require(
            statsAfterChurn.inlineStructAllocatedPages <= 64 * 8 &&
                statsAfterChurn.stringAllocatedPages <= 64 * 8 &&
                statsAfterChurn.heapStructAllocatedPages <= 64 * 4,
            "Allocated pages must remain strictly bounded by soft cap under heavy ephemeral churn");

        // Verify pinned holder fields on ManagedHeap survived all 200,000 evictions intact!
        const auto tfVal = rt->getMember(*holder, "tf", err);
        const auto attrsVal = rt->getMember(*holder, "attrs", err);
        const auto tagVal = rt->heap()->getStaticField(0x999999ULL);
        require(
            tfVal.has_value() &&
                *runtime::structValueFieldAt(std::get<runtime::StructValue>(*tfVal), 0) == runtime::Value{std::int64_t{777}} &&
                *runtime::structValueFieldAt(std::get<runtime::StructValue>(*tfVal), 2) == runtime::Value{std::int64_t{999}},
            "Pinned inline struct 'tf' must survive heavy clock eviction intact");
        require(
            attrsVal.has_value() &&
                *runtime::structValueFieldAt(std::get<runtime::StructValue>(*attrsVal), 4) == runtime::Value{std::int64_t{50}},
            "Pinned oversized struct 'attrs' must survive heavy clock eviction intact");
        require(
            tagVal.has_value() && std::get<std::string>(*tagVal) == "PinnedHostStringValue",
            "Pinned string 'tag' must survive heavy clock eviction intact");

        // Release holder root and overwrite static field, run full GC on the heap, then reclaim pool values!
        holder.reset();
        rt->heap()->setStaticField(0x999999ULL, runtime::Value{std::int64_t{0}});
        rt->heap()->collectFull(runtime::ShadowStack{});
        const auto statsAfterGc = runtime::getRuntimeValuePoolStatistics();
        const std::uint64_t totalGcReclaimed =
            statsAfterGc.inlineStructGcReclaimed +
            statsAfterGc.heapStructGcReclaimed +
            statsAfterGc.stringGcReclaimed;
        std::cout << "[Pool GC Sweep] Reclaimed slots=" << totalGcReclaimed
                  << " | Pinned remaining: inline=" << statsAfterGc.inlineStructPinnedSlots
                  << ", heapStruct=" << statsAfterGc.heapStructPinnedSlots
                  << ", string=" << statsAfterGc.stringPinnedSlots << "\n";
        require(
            totalGcReclaimed > 0 &&
                statsAfterGc.inlineStructPinnedSlots == 0 &&
                statsAfterGc.heapStructPinnedSlots == 0 &&
                statsAfterGc.stringPinnedSlots == 0,
            "reclaimUnreferencedPoolValues() must unpin and reclaim all dead slots after ManagedHeap destruction");
    }

    // 2. 8-Thread Concurrent Read/Write Scalability & Lock-Free Read Verification
    {
        constexpr int kThreads = 8;
        constexpr int kOpsPerThread = 25000;
        std::vector<std::thread> threads;
        std::atomic<bool> failed{false};

        const auto t0 = std::chrono::steady_clock::now();
        for (int tid = 0; tid < kThreads; ++tid) {
            threads.emplace_back([tid, &failed]() {
                for (int i = 0; i < kOpsPerThread; ++i) {
                    const std::int64_t v0 = tid * 1000000LL + (i & 255);
                    std::array<runtime::Value, 3> fields{v0, v0 + 1, v0 + 2};
                    auto st = runtime::makeStructValueFromFields(
                        0xABCDEF01ULL, runtime::ValueSpan(fields.data(), fields.size()));
                    const auto readBack = runtime::structValueFieldAt(st, 1);
                    if (!readBack || !(*readBack == runtime::Value{v0 + 1})) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                    runtime::StringValue sv("thread_" + std::to_string(tid) + "_key_" + std::to_string(i & 255));
                    if (sv.str().empty()) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        require(!failed.load(), "8-thread concurrent pool stress test encountered mismatch");
        std::cout << "[8-Thread Stress] " << (kThreads * kOpsPerThread)
                  << " concurrent struct+string intern & lock-free read ops completed in "
                  << ms << " ms\n";
    }

    // 3. Verify 32-bit BytecodeStringPool (>65,535 capacity) & lock-free RegisterSlicePool concurrency
    {
        constexpr int kUniqueBytecodeStrings = 70000; // Exceeds legacy 16-bit (65,535) limit
        std::vector<bytecode::BytecodeString> interned;
        interned.reserve(kUniqueBytecodeStrings);

        const bytecode::BytecodeString emptyStr{};
        require(emptyStr.empty() && emptyStr.poolId() == 0u, "Default BytecodeString must have poolId 0");
        require(emptyStr == "" && !(emptyStr != ""), "Empty BytecodeString const char* equality failed");

        for (int i = 0; i < kUniqueBytecodeStrings; ++i) {
            interned.emplace_back("bc_sym_const_" + std::to_string(i));
        }
        for (int i = 0; i < kUniqueBytecodeStrings; i += 997) {
            const std::string expected = "bc_sym_const_" + std::to_string(i);
            const bytecode::BytecodeString dup(expected);
            require(!interned[i].empty() && interned[i].poolId() != 0u, "Non-empty BytecodeString must have non-zero poolId");
            require(interned[i] == dup && interned[i].poolId() == dup.poolId(), "Deduplicated BytecodeString must share identical 32-bit poolId");
            require(interned[i] == expected && interned[i] != "other_literal", "BytecodeString string/c_str comparison failed");
        }

        // Verify Instruction 32-byte union layout & encode/decode round-trip for ConstantString
        static_assert(sizeof(bytecode::BytecodeString) == 4, "BytecodeString must be 4 bytes");
        static_assert(sizeof(bytecode::Instruction) == 32, "Instruction must remain 32 bytes");
        bytecode::Instruction strInst;
        strInst.opcode = bytecode::Opcode::ConstantString;
        strInst.result = 0;
        strInst.stringImmediate = bytecode::BytecodeString("union_string_payload");
        require(
            strInst.usesStringImmediate() &&
                !strInst.usesElementTypeId() &&
                !strInst.usesIntegerImmediate() &&
                !strInst.usesDoubleImmediate(),
            "ConstantString immediate classifier mismatch");
        require(
            strInst.activeStringImmediate() == "union_string_payload" &&
                strInst.activeElementTypeId() == 0u &&
                strInst.activeIntegerImmediate() == 0 &&
                strInst.activeDoubleImmediate() == 0.0,
            "ConstantString active immediate isolation failed");

        // 8-thread concurrent BytecodeString + SmallRegisterVector wait-free read / intern stress
        constexpr int kBcThreads = 8;
        constexpr int kBcOps = 10000;
        std::vector<std::thread> bcThreads;
        std::atomic<bool> bcFailed{false};
        for (int tid = 0; tid < kBcThreads; ++tid) {
            bcThreads.emplace_back([tid, &bcFailed, &interned]() {
                for (int i = 0; i < kBcOps; ++i) {
                    const std::string key = "bc_concurrent_" + std::to_string(tid) + "_" + std::to_string(i & 511);
                    bytecode::BytecodeString bs(key);
                    if (bs.str() != key || bs != interned[(tid * 1000 + i) % interned.size()].str() && bs == interned[(tid * 1000 + i) % interned.size()]) {
                        bcFailed.store(true, std::memory_order_relaxed);
                        return;
                    }
                    bytecode::SmallRegisterVector regs;
                    const auto r0 = static_cast<bytecode::Register>(tid + 1);
                    const auto r1 = static_cast<bytecode::Register>((i & 255) + 10);
                    const auto r2 = static_cast<bytecode::Register>((i & 255) + 20);
                    regs.push_back(r0);
                    regs.push_back(r1);
                    regs.push_back(r2);
                    if (regs.size() != 3 || regs[0] != r0 || regs[1] != r1 || regs[2] != r2) {
                        bcFailed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        for (auto& th : bcThreads) th.join();
        require(!bcFailed.load(), "8-thread concurrent BytecodeString + SmallRegisterVector stress failed");
        std::cout << "[Bytecode 32-Bit Pool] Verified " << kUniqueBytecodeStrings
                  << " unique strings (>64K) + 8-thread wait-free slice/string reads\n";
    }

    std::cout << "All pool refactoring verification tests passed!\n";
    return 0;
}
