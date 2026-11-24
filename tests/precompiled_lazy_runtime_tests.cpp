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
    require(afterExecCompiled > 0u,
        "Expected some lazy base functions to be compiled after execution");
    require(afterExecCompiled < totalLazy,
        "Expected uncalled lazy base functions to remain uncompiled (compiled=" +
        std::to_string(afterExecCompiled) + ", total=" + std::to_string(totalLazy) + ")");

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
    require(img->decodedLazyChunkCacheSize() <= 16u,
        "LRU chunk cache exceeded bound 16: " + std::to_string(img->decodedLazyChunkCacheSize()));
    require(img->evictedLazyChunkCount() >= 5u,
        "Expected LRU chunk eviction across 22 distinct chunks, got " +
        std::to_string(img->evictedLazyChunkCount()));
}

} // namespace

int main() {
    try {
        testLz4CodecRoundTripAndCorruption();
        testPrecompiledFrontendPackAndLayeredLazyRuntime();
        testConsumeHeaderAndLruChunkEvictionAndLazyGenericsSkip();
        std::cout << "All precompiled_lazy_runtime_tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << "\n";
        return 1;
    }
}
