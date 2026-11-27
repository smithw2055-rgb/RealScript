#include "realscript/bytecode/Bytecode.h"
#include "realscript/bytecode/Lz4Codec.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/PrecompiledModule.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string formatDiagnostics(const diagnostics::DiagnosticBag& bag) {
    std::ostringstream oss;
    for (const auto& d : bag.items()) {
        oss << d.sourceName << " [" << d.code << "] " << d.message << '\n';
    }
    return oss.str();
}

std::vector<bytecode::Module> lowerMirModules(
    const std::vector<mir::Module>& mirModules) {
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> out;
    out.reserve(mirModules.size());
    for (const auto& m : mirModules) {
        auto bc = lowerer.lower(m);
        diagnostics::DiagnosticBag diag;
        require(bytecode::verifyModule(bc, diag) && !diag.hasErrors(),
            "Bytecode verify failed for " + m.name + ":\n" + formatDiagnostics(diag));
        out.push_back(std::move(bc));
    }
    return out;
}

void testLz4CodecRoundTripAndCorruption() {
    // 1. Empty
    {
        std::vector<std::uint8_t> comp;
        require(bytecode::lz4CompressBlock(nullptr, 0, comp), "lz4 empty compress failed");
        require(comp.empty(), "lz4 empty output must be empty");
        std::vector<std::uint8_t> decomp;
        require(bytecode::lz4DecompressBlock(nullptr, 0, 0, decomp), "lz4 empty decompress failed");
        require(decomp.empty(), "lz4 empty decompressed must be empty");
    }

    // 2. Small literals (< 16 bytes)
    {
        const std::vector<std::uint8_t> input = {1, 2, 3, 4, 5, 6, 7};
        std::vector<std::uint8_t> comp;
        require(bytecode::lz4CompressBlock(input.data(), input.size(), comp), "lz4 small compress failed");
        std::vector<std::uint8_t> decomp;
        require(bytecode::lz4DecompressBlock(comp.data(), comp.size(), input.size(), decomp),
            "lz4 small decompress failed");
        require(decomp == input, "lz4 small round-trip mismatch");
    }

    // 3. Repetitive data (high compression ratio)
    {
        std::vector<std::uint8_t> input(32768u);
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<std::uint8_t>('A' + (i % 13u));
        }
        std::vector<std::uint8_t> comp;
        require(bytecode::lz4CompressBlock(input.data(), input.size(), comp),
            "lz4 repetitive compress failed");
        require(comp.size() < input.size() / 4u, "lz4 should compress repetitive input well");

        std::vector<std::uint8_t> decomp;
        require(bytecode::lz4DecompressBlock(comp.data(), comp.size(), input.size(), decomp),
            "lz4 repetitive decompress failed");
        require(decomp == input, "lz4 repetitive round-trip mismatch");

        // Wrong expected size or truncated input rejected
        require(!bytecode::lz4DecompressBlock(comp.data(), comp.size(), input.size() + 1u, decomp),
            "lz4 must reject wrong expected size");
        if (comp.size() > 2u) {
            require(!bytecode::lz4DecompressBlock(comp.data(), comp.size() / 2u, input.size(), decomp),
                "lz4 must reject truncated input");
        }
    }

    // 4. Pseudo-random incompressible binary data
    {
        std::vector<std::uint8_t> input(4096u);
        std::uint32_t state = 0x12345678u;
        for (std::size_t i = 0; i < input.size(); ++i) {
            state = state * 1664525u + 1013904223u;
            input[i] = static_cast<std::uint8_t>(state >> 24u);
        }
        std::vector<std::uint8_t> comp;
        require(bytecode::lz4CompressBlock(input.data(), input.size(), comp),
            "lz4 random compress failed");
        std::vector<std::uint8_t> decomp;
        require(bytecode::lz4DecompressBlock(comp.data(), comp.size(), input.size(), decomp),
            "lz4 random decompress failed");
        require(decomp == input, "lz4 random round-trip mismatch");
    }
}

void testPrecompiledFrontendPackAndLayeredLazyRuntime() {
    const std::string baseSource = R"RS(
module Precompiled.Base;

public enum Kind {
    Alpha = 1,
    Beta = 2,
}

public delegate int TransformInt(int x);

public interface IScorer {
    int score(int bonus);
}

public class BaseNode {
    public int baseVal;

    public BaseNode(int v) {
        baseVal = v;
    }

    public virtual int compute(int k) {
        return baseVal + k;
    }

    public int applyTwice(TransformInt fn, int x) {
        return fn(fn(x));
    }

    public int unusedHelperA(int x) {
        return x * 17 + 3;
    }
}

public class Box<T> {
    public T item;
    public int tag;

    public Box(T value, int tagVal) {
        item = value;
        tag = tagVal;
    }

    public T getItem() {
        return item;
    }
}

public class GenericHelper {
    public static T identity<T>(T value) {
        return value;
    }
}

public class Container {
    public int prefix;

    public Container(int p) {
        prefix = p;
    }

    public int offset() {
        return prefix;
    }

    public Box<T> wrap<T>(T val) {
        return new Box<T>(val, offset() + prefix);
    }

    public int neverCalledMethod(int n) {
        int acc = 0;
        for (int i = 0; i < n; ++i) {
            acc = acc + i * 3;
        }
        return acc;
    }
}

public int neverCalledFreeA(int x) {
    return x + 101;
}

public int neverCalledFreeB(int x) {
    if (x < 0) {
        throw new BaseFault("negative");
    }
    return x * x + 7;
}

public class BaseFault {
    public string message;
    public BaseFault(string m) {
        message = m;
    }
}

public class UnusedExtraType {
    public int z;
    public int unusedMethod(int x) {
        return x + z;
    }
}

public string baseBanner() {
    return "BaseBanner:" + "OK";
}

public int checkedDivide(int a, int b) {
    if (b == 0) {
        throw new BaseFault("divide by zero");
    }
    return a / b;
}
)RS";

    // Build Base with capturePrecompiledFrontend = true
    compiler::CompilationOptions baseOpts;
    baseOpts.capturePrecompiledFrontend = true;
    baseOpts.emitDebugInfo = true;
    compiler::Compilation baseComp({{"base.rscript", baseSource}}, baseOpts);
    auto baseBuild = baseComp.build();
    require(!baseBuild.diagnostics.hasErrors(),
        "Base compile failed:\n" + formatDiagnostics(baseBuild.diagnostics));
    require(baseBuild.precompiledFrontend != nullptr,
        "Expected precompiledFrontend pack to be captured");
    require(baseBuild.precompiledFrontend->findModule("Precompiled.Base") != nullptr,
        "Precompiled.Base must exist in captured PrecompiledFrontendPack");

    // 1. Encode & decode RSFP + corruption checks
    const std::vector<std::uint8_t> rsfpBytes =
        compiler::encodePrecompiledFrontendPack(*baseBuild.precompiledFrontend);
    require(rsfpBytes.size() > 16u, "Encoded RSFP pack should be non-trivial");

    {
        compiler::PrecompiledFrontendPack badPack;
        std::string err;
        std::vector<std::uint8_t> badMagic = rsfpBytes;
        badMagic[0] = 'X';
        require(!compiler::decodePrecompiledFrontendPack(badMagic, badPack, &err),
            "RSFP decoder must reject bad magic");
        require(!err.empty(), "Expected error string on bad magic");

        std::vector<std::uint8_t> badChecksum = rsfpBytes;
        badChecksum.back() ^= 0xFFu;
        err.clear();
        require(!compiler::decodePrecompiledFrontendPack(badChecksum, badPack, &err),
            "RSFP decoder must reject bad checksum");

        std::vector<std::uint8_t> truncated(rsfpBytes.begin(), rsfpBytes.begin() + 12);
        require(!compiler::decodePrecompiledFrontendPack(truncated, badPack, &err),
            "RSFP decoder must reject truncated pack");
    }

    auto decodedPack = std::make_shared<compiler::PrecompiledFrontendPack>();
    std::string decodeErr;
    require(compiler::decodePrecompiledFrontendPack(rsfpBytes, *decodedPack, &decodeErr),
        "Failed to decode valid RSFP: " + decodeErr);
    const auto* baseFrontBefore = decodedPack->findModule("Precompiled.Base");
    require(baseFrontBefore != nullptr, "Decoded pack missing Precompiled.Base");
    require(baseFrontBefore->typeLayer != nullptr, "Decoded pack missing lazy typeLayer");

    // Verify RSFP v2 starts with 0 materialized types and materializes lazily on demand
    require(decodedPack->totalIndexedTypeCount() >= 4u,
        "Expected >= 4 indexed types in Precompiled.Base");
    require(decodedPack->materializedTypeCount() == 0u,
        "Expected 0 materialized types immediately after RSFP v2 directory decode");

    const auto containerRefBefore = baseFrontBefore->typeLayer->findByName("Container", false);
    require(containerRefBefore.hasValue(), "Missing Container TypeSymbol in precompiled frontend");
    const std::size_t containerMethodsBefore = containerRefBefore->methods.size();
    require(decodedPack->materializedTypeCount() >= 1u &&
                decodedPack->materializedTypeCount() < decodedPack->totalIndexedTypeCount(),
        "Expected partial lazy materialization after looking up Container");

    // 2. Encode & decode LazyBytecodeBundle (small chunk target to force multiple chunks)
    std::vector<bytecode::Module> baseBcModules = lowerMirModules(baseBuild.modules);
    const std::vector<std::uint8_t> rslbBytes =
        bytecode::encodeLazyBytecodeBundle(baseBcModules, /*targetChunkBytes=*/256);
    require(rslbBytes.size() > 40u, "Encoded RSLB bundle should be non-trivial");

    {
        bytecode::LazyBytecodeBundle badBundle;
        diagnostics::DiagnosticBag badDiag;
        std::vector<std::uint8_t> badMagic = rslbBytes;
        badMagic[0] = 'Z';
        require(!bytecode::decodeLazyBytecodeBundleHeader(
                    badMagic.data(), badMagic.size(), badBundle, false, badDiag),
            "RSLB decoder must reject bad magic");
    }

    auto lazyBundle = std::make_shared<bytecode::LazyBytecodeBundle>();
    {
        diagnostics::DiagnosticBag rslbDiag;
        require(bytecode::decodeLazyBytecodeBundleHeader(
                    rslbBytes.data(), rslbBytes.size(), *lazyBundle, /*copyBlob=*/false, rslbDiag),
            "Failed to decode valid RSLB header:\n" + formatDiagnostics(rslbDiag));
        require(!rslbDiag.hasErrors(), "Unexpected RSLB diagnostics");
        require(lazyBundle->chunks.size() >= 2u,
            "Expected multiple RSLB chunks with small targetChunkBytes");

        // Verify corrupt chunk checksum is caught by decompressLazyChunk
        std::vector<std::uint8_t> corruptedBlob = rslbBytes;
        corruptedBlob.back() ^= 0x55u;
        bytecode::LazyBytecodeBundle corruptedView;
        diagnostics::DiagnosticBag cDiag;
        require(bytecode::decodeLazyBytecodeBundleHeader(
                    corruptedBlob.data(), corruptedBlob.size(), corruptedView, false, cDiag),
            "Header decode itself should still succeed");
        std::vector<std::uint8_t> chunkOut;
        require(!bytecode::decompressLazyChunk(
                    corruptedView, static_cast<std::uint32_t>(corruptedView.chunks.size() - 1u), chunkOut, cDiag),
            "decompressLazyChunk must reject corrupted chunk");
    }

    // 3. Compile User module against precompiledModules WITHOUT supplying Base source!
    const std::string userSource = R"RS(
module User.App;

import Precompiled.Base;

public class DerivedNode : BaseNode, IScorer {
    public int extra;

    public DerivedNode(int v, int e) : base(v) {
        extra = e;
    }

    public override int compute(int k) {
        return base.compute(k) + extra * 2;
    }

    public int score(int bonus) {
        return compute(bonus) + 5;
    }
}

int addThree(int x) {
    return x + 3;
}

public int RunScenario() {
    BaseNode baseRef = new DerivedNode(10, 4);
    int virtualRes = baseRef.compute(2); // 10 + 2 + 8 = 20

    IScorer scorer = (DerivedNode)baseRef;
    int ifaceRes = scorer.score(1); // (10 + 1 + 8) + 5 = 24

    TransformInt fn = addThree;
    int delRes = baseRef.applyTwice(fn, 7); // 13

    var intBox = new Box<int>(42, 2);
    int boxVal = intBox.getItem(); // 42
    Kind k = Kind.Beta;

    var strBox = new Box<string>("hello", 9);
    string s = GenericHelper.identity<string>(strBox.getItem());

    Container c = new Container(6);
    var wrapped = c.wrap<int>(15);
    int wrappedVal = wrapped.getItem() + wrapped.tag; // 15 + 12 = 27

    string banner = baseBanner();
    int divOk = checkedDivide(40, 5); // 8

    int caught = 0;
    try {
        int bad = checkedDivide(10, 0);
        caught = bad;
    } catch (BaseFault fault) {
        if (fault.message == "divide by zero") {
            caught = 100;
        }
    }

    if (k == Kind.Beta && s == "hello" && banner == "BaseBanner:OK") {
        return virtualRes + ifaceRes + delRes + boxVal + wrappedVal + divOk + caught;
        // 20 + 24 + 13 + 42 + 27 + 8 + 100 = 234
    }
    return -1;
}
)RS";

    compiler::CompilationOptions userOpts;
    userOpts.precompiledModules = decodedPack;
    userOpts.emitDebugInfo = true;
    compiler::Compilation userComp({{"user.rscript", userSource}}, userOpts);
    auto userBuild = userComp.build();
    require(!userBuild.diagnostics.hasErrors(),
        "User compile against PrecompiledFrontendPack failed:\n" +
        formatDiagnostics(userBuild.diagnostics));

    for (const auto& mm : userBuild.modules) {
        require(mm.name != "Precompiled.Base",
            "User build output must not re-emit Precompiled.Base module");
    }
    require(containerRefBefore->methods.size() == containerMethodsBefore,
        "Compiling member generic specialization must NOT mutate precompiled TypeSymbol::methods");
    require(decodedPack->materializedTypeCount() < decodedPack->totalIndexedTypeCount(),
        "Expected lazy RSFP v2 to skip materializing unreferenced Base types (mat=" +
        std::to_string(decodedPack->materializedTypeCount()) + ", total=" +
        std::to_string(decodedPack->totalIndexedTypeCount()) + ")");

    std::vector<bytecode::Module> userBcModules = lowerMirModules(userBuild.modules);

    // 4. Link layered (LazyBytecodeBundle + user bytecode) vs eager baseline
    runtime::RuntimeError linkErr;
    auto layeredOpt = runtime::ProgramImage::linkLayered(
        lazyBundle, userBcModules, linkErr, /*verifyUserModules=*/true);
    require(layeredOpt.has_value(),
        "ProgramImage::linkLayered failed: " + linkErr.message);

    auto layeredImg = std::make_shared<runtime::ProgramImage>(std::move(*layeredOpt));
    const std::size_t totalLazy = layeredImg->totalLazyFunctionCount();
    const std::size_t beforeExecCompiled = layeredImg->compiledLazyFunctionCount();
    require(totalLazy >= 10u, "Expected multiple lazy base functions");
    require(beforeExecCompiled == 0u,
        "Expected zero lazy base functions compiled immediately after linkLayered, got " +
        std::to_string(beforeExecCompiled));

    // Also test ProgramImage copy constructor preserves lazy state before execution
    auto copiedLazyImg = std::make_shared<runtime::ProgramImage>(*layeredImg);
    require(copiedLazyImg->totalLazyFunctionCount() == totalLazy,
        "Copied ProgramImage should keep totalLazyFunctionCount");
    require(copiedLazyImg->compiledLazyFunctionCount() == 0u,
        "Copied ProgramImage should start with 0 compiled lazy functions");

    // Eager baseline for exact parity check
    std::vector<bytecode::Module> eagerAllModules = baseBcModules;
    eagerAllModules.insert(
        eagerAllModules.end(), userBcModules.begin(), userBcModules.end());
    auto eagerOpt = runtime::ProgramImage::link(std::move(eagerAllModules), linkErr, true);
    require(eagerOpt.has_value(), "Eager ProgramImage::link failed: " + linkErr.message);
    auto eagerImg = std::make_shared<const runtime::ProgramImage>(std::move(*eagerOpt));

    runtime::Interpreter lazyInterp(layeredImg);
    auto lazyRes = lazyInterp.invoke("User.App::RunScenario");
    require(lazyRes.succeeded,
        "Lazy layered execution failed: " + lazyRes.error.message);

    runtime::Interpreter eagerInterp(eagerImg);
    auto eagerRes = eagerInterp.invoke("User.App::RunScenario");
    require(eagerRes.succeeded,
        "Eager baseline execution failed: " + eagerRes.error.message);

    std::int64_t lazyInt = 0;
    std::int64_t eagerInt = 0;
    require(lazyRes.value.tryGetInt64(lazyInt) && lazyInt == 234,
        "Unexpected lazy return value: " + std::to_string(lazyInt));
    require(eagerRes.value.tryGetInt64(eagerInt) && eagerInt == lazyInt,
        "Eager vs lazy return value mismatch");

    const std::size_t afterExecCompiled = layeredImg->compiledLazyFunctionCount();
    const std::size_t afterExecMaterialized = layeredImg->materializedExecutionFunctionCount();
    require(afterExecCompiled > 0u,
        "Expected some lazy base functions to be compiled after execution");
    require(afterExecCompiled < totalLazy,
        "Expected uncalled lazy base functions to remain uncompiled (compiled=" +
        std::to_string(afterExecCompiled) + ", total=" + std::to_string(totalLazy) + ")");
    require(afterExecMaterialized > 0u && afterExecMaterialized < layeredImg->functionCount(),
        "Expected unreferenced lazy base functions to remain unmaterialized (mat=" +
        std::to_string(afterExecMaterialized) + ", total=" +
        std::to_string(layeredImg->functionCount()) + ")");

    // Re-running on same interpreter + copied image should also return 234 identically
    auto lazyRes2 = lazyInterp.invoke("User.App::RunScenario");
    std::int64_t lazyInt2 = 0;
    require(lazyRes2.succeeded && lazyRes2.value.tryGetInt64(lazyInt2) && lazyInt2 == 234,
        "Second invoke on lazy interpreter failed");

    runtime::Interpreter copiedInterp(copiedLazyImg);
    auto copiedRes = copiedInterp.invoke("User.App::RunScenario");
    std::int64_t copiedInt = 0;
    require(copiedRes.succeeded && copiedRes.value.tryGetInt64(copiedInt) && copiedInt == 234,
        "Invoke on copied ProgramImage failed");
}

void testConsumeHeaderAndLruChunkEvictionAndLazyGenericsSkip() {
    // A. Build a module with > 20 distinct helper functions so 1-fn chunks force > 16 chunks & LRU eviction
    std::ostringstream bigSrc;
    bigSrc << "module Precompiled.LruMod;\n"
           << "public class UnusedHuge {\n"
           << "    public int a; public int b; public int c;\n"
           << "    public int calc(int x) { return x + a + b + c; }\n"
           << "}\n"
           << "public class ExtraT1 { public int v; }\n"
           << "public class ExtraT2 { public int v; }\n"
           << "public class ExtraT3 { public int v; }\n"
           << "public class Boxed<T> { public T v; public Boxed(T x) { v = x; } }\n";
    for (int i = 0; i < 22; ++i) {
        bigSrc << "public int fn_" << i << "(int x) { return x + " << i << "; }\n";
    }

    compiler::CompilationOptions baseOpts;
    baseOpts.capturePrecompiledFrontend = true;
    compiler::Compilation baseComp({{"lru_base.rscript", bigSrc.str()}}, baseOpts);
    auto baseBuild = baseComp.build();
    require(!baseBuild.diagnostics.hasErrors(), "LRU base compile failed");

    const auto rsfp = compiler::encodePrecompiledFrontendPack(*baseBuild.precompiledFrontend);
    auto pack = std::make_shared<compiler::PrecompiledFrontendPack>();
    std::string err;
    require(compiler::decodePrecompiledFrontendPack(rsfp.data(), rsfp.size(), *pack, &err, false, false),
        "Zero-copy RSFP decode failed: " + err);
    require(pack->materializedTypeCount() == 0u, "RSFP must start with 0 materialized types");

    // Non-generic user module must NOT materialize unrelated types or parse Boxed<T>
    const std::string nonGenericUser = R"RS(
module User.LruRunner;
import Precompiled.LruMod;
public int CallAll() {
    int sum = 0;
)RS" + []() {
        std::ostringstream s;
        for (int i = 0; i < 22; ++i) s << "    sum = sum + fn_" << i << "(1);\n";
        s << "    return sum;\n}\n";
        return s.str();
    }();

    compiler::CompilationOptions userOpts;
    userOpts.precompiledModules = pack;
    compiler::Compilation userComp({{"user_lru.rscript", nonGenericUser}}, userOpts);
    auto userBuild = userComp.build();
    require(!userBuild.diagnostics.hasErrors(),
        "LRU user compile failed:\n" + formatDiagnostics(userBuild.diagnostics));
    require(pack->materializedTypeCount() == 0u,
        "Non-generic free-function user module should materialize 0 class types, got " +
        std::to_string(pack->materializedTypeCount()));

    auto baseBc = lowerMirModules(baseBuild.modules);
    auto userBc = lowerMirModules(userBuild.modules);

    // targetChunkBytes = 0 puts each function in its own chunk (> 22 chunks)
    const auto rslb = bytecode::encodeLazyBytecodeBundle(baseBc, /*targetChunkBytes=*/0u);
    auto mutableBundle = std::make_shared<bytecode::LazyBytecodeBundle>();
    diagnostics::DiagnosticBag bDiag;
    require(bytecode::decodeLazyBytecodeBundleHeader(
                rslb.data(), rslb.size(), *mutableBundle, /*copyBlob=*/true, bDiag, /*skipChecksum=*/true),
        "RSLB consume-header decode failed");
    require(mutableBundle->chunks.size() >= 22u,
        "Expected >= 22 chunks when targetChunkBytes=0");

    runtime::RuntimeError linkErr;
    auto consumedOpt = runtime::ProgramImage::linkLayeredConsumeHeader(
        mutableBundle, std::move(userBc), linkErr, true);
    require(consumedOpt.has_value(), "linkLayeredConsumeHeader failed: " + linkErr.message);
    require(mutableBundle->headerModules.empty(),
        "Expected linkLayeredConsumeHeader to move & clear headerModules");

    consumedOpt->compactForRuntime();
    auto img = std::make_shared<const runtime::ProgramImage>(std::move(*consumedOpt));
    runtime::Interpreter interp(img);
    auto res = interp.invoke("User.LruRunner::CallAll");
    std::int64_t val = 0;
    require(res.succeeded && res.value.tryGetInt64(val) && val == 22 + 231,
        "LRU consume-header invoke failed");
    require(img->decodedLazyChunkCacheSize() <= 4u,
        "LRU chunk cache exceeded bound 4: " + std::to_string(img->decodedLazyChunkCacheSize()));
    require(img->evictedLazyChunkCount() >= 5u,
        "Expected LRU chunk eviction across 22 distinct chunks, got " +
        std::to_string(img->evictedLazyChunkCount()));
}

void testM1LazyV3AndRuntimeOptimizations() {
    auto writeU32Le = [](std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) {
        buf[off + 0] = static_cast<std::uint8_t>(v & 0xFFu);
        buf[off + 1] = static_cast<std::uint8_t>((v >> 8u) & 0xFFu);
        buf[off + 2] = static_cast<std::uint8_t>((v >> 16u) & 0xFFu);
        buf[off + 3] = static_cast<std::uint8_t>((v >> 24u) & 0xFFu);
    };
    auto appendU32Le = [](std::vector<std::uint8_t>& buf, std::uint32_t v) {
        buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        buf.push_back(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
        buf.push_back(static_cast<std::uint8_t>((v >> 16u) & 0xFFu));
        buf.push_back(static_cast<std::uint8_t>((v >> 24u) & 0xFFu));
    };

    // 1. Lazy polymorphic dispatch, interface dispatch, NewObject templates, and string predicates
    std::ostringstream polyBaseSrc;
    polyBaseSrc << R"RS(
module Precompiled.PolyBase;

public interface IMeter {
    int measure();
}

public class WidgetBase : IMeter {
    public int tag = 10;
    public WidgetBase(int t) { tag = t; }
    public virtual int score() { return tag + 1; }
    public int measure() { return score(); }
}

public class WidgetDerived : WidgetBase {
    public int extra = 20;
    public WidgetDerived(int t, int e) : base(t) { extra = e; }
    public override int score() { return tag + extra; }
}

public WidgetBase makeWidget(bool derived, int a, int b) {
    if (derived) return new WidgetDerived(a, b);
    return new WidgetBase(a);
}

public int measureMeter(IMeter m) {
    return m.measure();
}

public int checkLazyExtra(int x) {
    return x * 3 + 7;
}

public int neverReferencedLazyHelper(int z) {
    return z * 7 + 13;
}

public class NeverReferencedLazyClass {
    public int v;
    public int compute() { return v + 1; }
}
)RS";

    std::ostringstream userSrc;
    userSrc << R"RS(
module User.M1Ops;
import Precompiled.PolyBase;

)RS";
    for (int i = 0; i < 80; ++i) {
        userSrc << "int hostForward" << i << "(int x);\n";
    }
    userSrc << R"RS(
public int polyC1() { WidgetBase b = makeWidget(false, 5, 0); return measureMeter(b); }
public int polyC2() { WidgetBase d = makeWidget(true, 5, 30); return measureMeter(d); }
public int polyC3() {
    WidgetBase b = makeWidget(false, 5, 0);
    WidgetBase d = makeWidget(true, 5, 30);
    return (d is WidgetDerived && !(b is WidgetDerived)) ? 100 : 0;
}
public int polyC4() {
    WidgetBase d = makeWidget(true, 5, 30);
    WidgetDerived casted = d as WidgetDerived;
    return (casted != null) ? casted.extra : 0;
}
public int runPolyChecks() {
    return polyC1() + polyC2() + polyC3() + polyC4();
}

public int runStringChecks(string prefixIn, string nullIn) {
    int total = 0;
    string hay = "alpha-beta-γδ";
    string emptyS = "";
    string pref = "alpha";
    string suff = "γδ";
    string mid = "-beta-";
    if (emptyS.IsEmpty() && !hay.IsEmpty()) {
        total = total + 1;
    }
    if (hay.StartsWith(pref) && hay.EndsWith(suff) && hay.Contains(mid) &&
        hay.StartsWith(emptyS) && hay.EndsWith(emptyS) && hay.Contains(emptyS) &&
        hay.Contains(hay) && hay.StartsWith(hay) && hay.EndsWith(hay) &&
        !hay.StartsWith("beta") && !hay.EndsWith("alpha") && !hay.Contains("zzz")) {
        total = total + 10;
    }
    if (hay.StartsWith(prefixIn) && prefixIn.Contains("-")) {
        total = total + 2;
    }
    if (nullIn != null && nullIn.IsEmpty()) {
        total = total + 1000;
    }
    return total;
}

public int runManyNatives() {
    int acc = 0;
)RS";
    for (int i = 0; i < 80; ++i) {
        userSrc << "    acc = acc + hostForward" << i << "(1);\n";
    }
    userSrc << R"RS(    return acc;
}

public int runAfterRelease() {
    return checkLazyExtra(10);
}

public bool checkNullStringThrows() {
    string s = null;
    return s.StartsWith("a");
}
)RS";

    compiler::Compilation baseComp({{"poly_base.rscript", polyBaseSrc.str()}}, {});
    auto baseBuild = baseComp.build();
    require(!baseBuild.diagnostics.hasErrors(),
        "Poly base compile failed:\n" + formatDiagnostics(baseBuild.diagnostics));
    auto baseBc = lowerMirModules(baseBuild.modules);

    const auto rslbV4 = bytecode::encodeLazyBytecodeBundle(baseBc, /*targetChunkBytes=*/0u);
    auto v4BundleZeroCopy = std::make_shared<bytecode::LazyBytecodeBundle>();
    diagnostics::DiagnosticBag diag;
    require(bytecode::decodeLazyBytecodeBundleHeader(
                rslbV4.data(), rslbV4.size(), *v4BundleZeroCopy, /*copyBlob=*/false, diag, /*skipChecksum=*/false),
        "RSLB v4 zero-copy header decode failed");
    require(rslbV4.size() >= 8u &&
            (static_cast<std::uint32_t>(rslbV4[4]) |
             (static_cast<std::uint32_t>(rslbV4[5]) << 8u) |
             (static_cast<std::uint32_t>(rslbV4[6]) << 16u) |
             (static_cast<std::uint32_t>(rslbV4[7]) << 24u)) == 4u,
        "Expected encodeLazyBytecodeBundle binary header version == 4u");
    require(v4BundleZeroCopy->sharedStrings.empty() &&
            v4BundleZeroCopy->flatStringSpans.empty() &&
            v4BundleZeroCopy->flatStringData.empty() &&
            v4BundleZeroCopy->functionLocs.empty(),
        "Expected RSLB v4 zero-copy decode to skip heap string/loc vectors");
    require(v4BundleZeroCopy->hasZeroCopyStrings() &&
            v4BundleZeroCopy->hasZeroCopyFunctionLocs() &&
            v4BundleZeroCopy->stringCount() > 0u &&
            v4BundleZeroCopy->flatStringBytes() > 0u &&
            v4BundleZeroCopy->functionLocCount() > 0u,
        "Expected RSLB v4 zero-copy string pool and locator views to be active");
    {
        const auto strPtrAddr = reinterpret_cast<std::uintptr_t>(v4BundleZeroCopy->flatStringDataPtr());
        const auto blobBeginAddr = reinterpret_cast<std::uintptr_t>(rslbV4.data());
        const auto blobEndAddr = blobBeginAddr + rslbV4.size();
        require(strPtrAddr >= blobBeginAddr && strPtrAddr < blobEndAddr,
            "Expected copyBlob=false string view pointer inside caller blob");
    }
    require(v4BundleZeroCopy->stringViewAt(0) == v4BundleZeroCopy->stringCopyAt(0),
        "Expected stringViewAt and stringCopyAt parity on v4 zero-copy bundle");

    auto v4BundleOwned = std::make_shared<bytecode::LazyBytecodeBundle>();
    require(bytecode::decodeLazyBytecodeBundleHeader(
                rslbV4.data(), rslbV4.size(), *v4BundleOwned, /*copyBlob=*/true, diag, /*skipChecksum=*/false),
        "RSLB v4 owned-blob header decode failed");
    require(v4BundleOwned->hasZeroCopyStrings() && v4BundleOwned->hasZeroCopyFunctionLocs(),
        "Expected copyBlob=true v4 bundle to use zero-copy views into ownedBlob");
    {
        const auto strPtrAddr = reinterpret_cast<std::uintptr_t>(v4BundleOwned->flatStringDataPtr());
        const auto ownBeginAddr = reinterpret_cast<std::uintptr_t>(v4BundleOwned->ownedBlob.data());
        const auto ownEndAddr = ownBeginAddr + v4BundleOwned->ownedBlob.size();
        require(strPtrAddr >= ownBeginAddr && strPtrAddr < ownEndAddr,
            "Expected copyBlob=true string view pointer inside ownedBlob");

        bytecode::LazyBytecodeBundle v4Copied(*v4BundleOwned);
        const auto copyStrAddr = reinterpret_cast<std::uintptr_t>(v4Copied.flatStringDataPtr());
        const auto copyOwnBegin = reinterpret_cast<std::uintptr_t>(v4Copied.ownedBlob.data());
        const auto copyOwnEnd = copyOwnBegin + v4Copied.ownedBlob.size();
        require(copyStrAddr >= copyOwnBegin && copyStrAddr < copyOwnEnd,
            "Expected copied LazyBytecodeBundle string view pointer inside copy.ownedBlob");
        require(v4Copied.stringViewAt(0) == v4BundleOwned->stringViewAt(0),
            "Expected copied v4 LazyBytecodeBundle stringViewAt parity");
    }

    // 2. Verify RSLB v2 and v3 header backward compatibility
    {
        std::vector<std::uint8_t> rawV2;
        appendU32Le(rawV2, 1u); // string count
        appendU32Le(rawV2, 4u); // "v2ok" len
        rawV2.push_back('v');
        rawV2.push_back('2');
        rawV2.push_back('o');
        rawV2.push_back('k');
        appendU32Le(rawV2, 0u); // moduleCount = 0
        appendU32Le(rawV2, 0u); // locCount = 0

        std::vector<std::uint8_t> compV2;
        require(bytecode::lz4CompressBlock(rawV2.data(), rawV2.size(), compV2),
            "Failed to compress synthetic v2 header");

        std::vector<std::uint8_t> rslbV2;
        rslbV2.insert(rslbV2.end(), rslbV4.begin(), rslbV4.begin() + 40);
        writeU32Le(rslbV2, 4u, 2u); // formatVersion = 2
        writeU32Le(rslbV2, 16u, 0u); // chunkCount = 0
        writeU32Le(rslbV2, 20u, 40u); // compHdrOffset = 40
        writeU32Le(rslbV2, 24u, static_cast<std::uint32_t>(compV2.size()));
        writeU32Le(rslbV2, 28u, static_cast<std::uint32_t>(rawV2.size()));
        writeU32Le(rslbV2, 32u, 0u);
        writeU32Le(rslbV2, 36u, 0u);
        rslbV2.insert(rslbV2.end(), compV2.begin(), compV2.end());

        bytecode::LazyBytecodeBundle v2Bundle;
        diagnostics::DiagnosticBag v2Diag;
        require(bytecode::decodeLazyBytecodeBundleHeader(
                    rslbV2.data(), rslbV2.size(), v2Bundle, /*copyBlob=*/true, v2Diag, /*skipChecksum=*/true),
            "RSLB v2 compat decode failed: " + formatDiagnostics(v2Diag));
        require(v2Bundle.flatStringSpans.empty() && !v2Bundle.hasZeroCopyStrings(),
            "Expected v2 bundle flatStringSpans empty");
        require(v2Bundle.sharedStrings.size() == 1u && v2Bundle.stringViewAt(0) == "v2ok",
            "Expected v2 bundle stringViewAt(0) == v2ok");
    }
    {
        std::vector<std::uint8_t> rawV3;
        appendU32Le(rawV3, 1u); // string count
        appendU32Le(rawV3, 4u); // total chars ("v3ok")
        appendU32Le(rawV3, 4u); // len[0]
        rawV3.push_back('v');
        rawV3.push_back('3');
        rawV3.push_back('o');
        rawV3.push_back('k');
        appendU32Le(rawV3, 0u); // moduleCount = 0
        appendU32Le(rawV3, 0u); // locCount = 0

        std::vector<std::uint8_t> compV3;
        require(bytecode::lz4CompressBlock(rawV3.data(), rawV3.size(), compV3),
            "Failed to compress synthetic v3 header");

        std::vector<std::uint8_t> rslbV3Compat;
        rslbV3Compat.insert(rslbV3Compat.end(), rslbV4.begin(), rslbV4.begin() + 40);
        writeU32Le(rslbV3Compat, 4u, 3u); // formatVersion = 3
        writeU32Le(rslbV3Compat, 16u, 0u); // chunkCount = 0
        writeU32Le(rslbV3Compat, 20u, 40u); // compHdrOffset = 40
        writeU32Le(rslbV3Compat, 24u, static_cast<std::uint32_t>(compV3.size()));
        writeU32Le(rslbV3Compat, 28u, static_cast<std::uint32_t>(rawV3.size()));
        writeU32Le(rslbV3Compat, 32u, 0u);
        writeU32Le(rslbV3Compat, 36u, 0u);
        rslbV3Compat.insert(rslbV3Compat.end(), compV3.begin(), compV3.end());

        bytecode::LazyBytecodeBundle v3CompatBundle;
        diagnostics::DiagnosticBag v3Diag;
        require(bytecode::decodeLazyBytecodeBundleHeader(
                    rslbV3Compat.data(), rslbV3Compat.size(), v3CompatBundle, /*copyBlob=*/true, v3Diag, /*skipChecksum=*/true),
            "RSLB v3 compat decode failed: " + formatDiagnostics(v3Diag));
        require(v3CompatBundle.sharedStrings.empty() &&
                v3CompatBundle.flatStringSpans.size() == 1u &&
                v3CompatBundle.stringViewAt(0) == "v3ok",
            "Expected v3 compat bundle stringViewAt(0) == v3ok");
    }

    // 3. Compile user module against PolyBase sources & link layered
    compiler::Compilation userComp(
        {{"poly_base_sig.rscript", polyBaseSrc.str()}, {"m1_user.rscript", userSrc.str()}},
        {});
    auto userAllBuild = userComp.build();
    require(!userAllBuild.diagnostics.hasErrors(),
        "M1 user compile failed:\n" + formatDiagnostics(userAllBuild.diagnostics));
    std::vector<bytecode::Module> userOnlyBc;
    for (const auto& m : lowerMirModules(userAllBuild.modules)) {
        if (m.name != "Precompiled.PolyBase") {
            userOnlyBc.push_back(m);
        }
    }

    runtime::RuntimeError linkErr;
    auto layeredOpt = runtime::ProgramImage::linkLayered(v4BundleOwned, userOnlyBc, linkErr, true);
    require(layeredOpt.has_value(), "M1 linkLayered failed: " + linkErr.message);
    auto img = std::make_shared<runtime::ProgramImage>(std::move(*layeredOpt));

    // Confirm findFunction zero-alloc lookup matches Precompiled.PolyBase::checkLazyExtra
    require(img->findFunction("Precompiled.PolyBase::checkLazyExtra").has_value(),
        "findFunction failed on Precompiled.PolyBase::checkLazyExtra");
    require(!img->findFunction("Missing.Module::nope").has_value(),
        "findFunction unexpectedly matched missing function");

    // Compare eager link vs lazy link on same modules
    std::vector<bytecode::Module> eagerMods = baseBc;
    eagerMods.insert(eagerMods.end(), userOnlyBc.begin(), userOnlyBc.end());
    auto eagerOpt = runtime::ProgramImage::link(std::move(eagerMods), linkErr, true);
    require(eagerOpt.has_value(), "Eager link failed: " + linkErr.message);
    auto eagerImg = std::make_shared<runtime::ProgramImage>(std::move(*eagerOpt));
    runtime::Interpreter eagerInterp(eagerImg);
    auto eagerPolyRes = eagerInterp.invoke("User.M1Ops::runPolyChecks");
    std::int64_t eagerPolyVal = 0;
    require(eagerPolyRes.succeeded && eagerPolyRes.value.tryGetInt64(eagerPolyVal) && eagerPolyVal == 171,
        "Eager runPolyChecks mismatch");

    runtime::Interpreter interp(img);
    auto polyRes = interp.invoke("User.M1Ops::runPolyChecks");
    std::int64_t polyVal = 0;
    const bool polyOk = polyRes.succeeded && polyRes.value.tryGetInt64(polyVal);
    require(polyOk && polyVal == 171,
        "Unexpected runPolyChecks result (tag=" +
        std::to_string(static_cast<int>(polyRes.value.rawTag())) + "): " +
        std::to_string(polyVal) + " err=" + polyRes.error.message);

    auto strRes = interp.invoke(
        "User.M1Ops::runStringChecks",
        {runtime::Value{std::string{"alpha-"}}, runtime::Value{runtime::NullString{}}});
    std::int64_t strVal = 0;
    const bool strOk = strRes.succeeded && strRes.value.tryGetInt64(strVal);
    require(strOk && strVal == 13,
        "Unexpected runStringChecks result (tag=" +
        std::to_string(static_cast<int>(strRes.value.rawTag())) + "): " +
        std::to_string(strVal) + " err=" + strRes.error.message);

    // Verify decoded lazy chunks > 0 then releaseTransientLazyCaches() -> 0, then call another lazy fn
    require(img->decodedLazyChunkCacheSize() > 0u && img->decodedLazyChunkCacheSize() <= 4u,
        "Expected decoded lazy chunks in (0, 4], got " + std::to_string(img->decodedLazyChunkCacheSize()));
    img->releaseTransientLazyCaches();
    require(img->decodedLazyChunkCacheSize() == 0u,
        "Expected releaseTransientLazyCaches() to reset cache size to 0");

    auto afterReleaseRes = interp.invoke("User.M1Ops::runAfterRelease");
    std::int64_t afterVal = 0;
    require(afterReleaseRes.succeeded && afterReleaseRes.value.tryGetInt64(afterVal) && afterVal == 37,
        "runAfterRelease after releaseTransientLazyCaches failed");
    require(img->decodedLazyChunkCacheSize() == 1u,
        "Expected post-release lazy execution to repopulate cache to 1 chunk");

    // 4. Null-string intrinsic must fail cleanly with NullReference
    auto nullRes = interp.invoke("User.M1Ops::checkNullStringThrows");
    require(!nullRes.succeeded && nullRes.error.code == runtime::ErrorCode::NullReference,
        "Expected NullReference error for null string StartsWith");

    // 5. Exercise >64 distinct native forwarder bindings across repeated invokes (512-entry TLS cache)
    auto bindings = std::make_shared<runtime::BindingRegistry>();
    for (int i = 0; i < 80; ++i) {
        const std::string bindName = "User.M1Ops::hostForward" + std::to_string(i);
        require(
            bindings->bind(
                bindName,
                [i](const bytecode::FunctionReference&,
                    runtime::ValueSpan args,
                    runtime::RuntimeError&) -> std::optional<runtime::Value> {
                    return runtime::Value{std::get<std::int64_t>(args[0]) + i};
                }),
            "Failed to bind " + bindName);
    }
    interp.setBindingRegistry(bindings);
    std::int64_t expectedNativeSum = 80; // 80 * 1 + sum(0..79)=3160 => 3240
    for (int i = 0; i < 80; ++i) expectedNativeSum += i;
    for (int round = 0; round < 3; ++round) {
        auto natRes = interp.invoke("User.M1Ops::runManyNatives");
        std::int64_t natVal = 0;
        require(natRes.succeeded && natRes.value.tryGetInt64(natVal) && natVal == expectedNativeSum,
            "runManyNatives mismatch on round " + std::to_string(round) + ": " + natRes.error.message);
    }

    const std::size_t materializedBeforeExtra = img->materializedExecutionFunctionCount();
    require(materializedBeforeExtra > 0u && materializedBeforeExtra < img->functionCount(),
        "Expected on-demand ExecutionMirror to skip uncalled lazy functions (mat=" +
        std::to_string(materializedBeforeExtra) + ", total=" +
        std::to_string(img->functionCount()) + ")");

    std::cout << "  [M1Stats] rslbV4Bytes=" << rslbV4.size()
              << " flatPoolBytes=" << v4BundleZeroCopy->flatStringBytes()
              << " flatStringCount=" << v4BundleZeroCopy->stringCount()
              << " materializedExecFns=" << materializedBeforeExtra << "/" << img->functionCount()
              << " decodedChunksAfterRelease=" << img->decodedLazyChunkCacheSize()
              << "\n";
}

} // namespace

int main() {
    try {
        testLz4CodecRoundTripAndCorruption();
        testPrecompiledFrontendPackAndLayeredLazyRuntime();
        testConsumeHeaderAndLruChunkEvictionAndLazyGenericsSkip();
        testM1LazyV3AndRuntimeOptimizations();
        std::cout << "All precompiled_lazy_runtime_tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << "\n";
        return 1;
    }
}
