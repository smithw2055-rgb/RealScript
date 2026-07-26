#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot read file: " + path);
    }
    std::ostringstream out;
    out << stream.rdbuf();
    return out.str();
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot write file: " + path);
    }
    stream << content;
}

std::string normalizeLineEndings(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\r') {
            if (index + 1 < value.size() && value[index + 1] == '\n') {
                ++index;
            }
            result.push_back('\n');
        } else {
            result.push_back(value[index]);
        }
    }
    return result;
}


void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    require(offset + 4 <= bytes.size(), "test byte patch is outside the buffer");
    for (int shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require(offset + 4 <= bytes.size(), "test byte read is outside the buffer");
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}
bool hasDiagnostic(
    const realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& code) {
    for (const auto& diagnostic : diagnostics.items()) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

realscript::bytecode::Module compileBytecode(
    std::vector<realscript::compiler::SourceFile> sources) {
    realscript::compiler::Compilation compilation(std::move(sources));
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(), "source compilation produced diagnostics");
    require(build.modules.size() == 1, "expected exactly one MIR module");

    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(
        realscript::bytecode::verifyModule(module, diagnostics),
        "lowered bytecode failed verification");
    return module;
}

realscript::bytecode::Module compileFixture() {
    const std::string root = REALSCRIPT_SOURCE_DIR;
    return compileBytecode({{
        "phase2a.rs",
        readFile(root + "/tests/fixtures/phase2a.rs"),
    }});
}

void testLoweringAndVerification() {
    const auto module = compileFixture();
    require(module.version.major == 0 && module.version.minor == 3,
            "unexpected bytecode version");
    require(module.functions.size() == 6, "fixture function count changed");
    require(!module.functionReferences.empty(), "calls must create function references");

    const auto text = realscript::bytecode::disassembleModule(module);
    require(text.find("call ref") != std::string::npos,
            "bytecode disassembly must contain a call");
    require(text.find("conv.null.string") != std::string::npos,
            "bytecode disassembly must contain explicit conversion");
    require(text.find("br r") != std::string::npos,
            "bytecode disassembly must contain a branch");
    require(text.find("bb2(r") != std::string::npos,
            "short-circuit merge must retain a block parameter");
}

void testRoundTripIsCanonical() {
    const auto source = compileFixture();
    const auto encoded = realscript::bytecode::encodeModule(source);

    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(
        realscript::bytecode::decodeModule(encoded, decoded, diagnostics),
        "valid encoded module failed to decode");
    require(!diagnostics.hasErrors(), "valid bytecode decode produced diagnostics");
    require(
        realscript::bytecode::verifyModule(decoded, diagnostics),
        "decoded bytecode failed verification");
    require(
        encoded == realscript::bytecode::encodeModule(decoded),
        "decode/re-encode must be byte-for-byte canonical");
    require(
        realscript::bytecode::disassembleModule(source) ==
            realscript::bytecode::disassembleModule(decoded),
        "round-trip changed bytecode semantics");
}

void testDecoderRejectsBadMagic() {
    auto bytes = realscript::bytecode::encodeModule(compileFixture());
    bytes[0] = 'X';
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::decodeModule(bytes, decoded, diagnostics),
            "bad magic must fail decoding");
    require(hasDiagnostic(diagnostics, "RS5000"),
            "bad magic must produce RS5000");
}

void testDecoderRejectsUnsupportedVersion() {
    auto bytes = realscript::bytecode::encodeModule(compileFixture());
    bytes[4] = 1;
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::decodeModule(bytes, decoded, diagnostics),
            "unsupported version must fail decoding");
    require(hasDiagnostic(diagnostics, "RS5001"),
            "unsupported version must produce RS5001");
}

void testDecoderRejectsTruncation() {
    auto bytes = realscript::bytecode::encodeModule(compileFixture());
    bytes.resize(bytes.size() - 7);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::decodeModule(bytes, decoded, diagnostics),
            "truncated module must fail decoding");
    require(diagnostics.hasErrors(), "truncated module must produce diagnostics");
}

void testDecoderRejectsSectionOverlap() {
    auto bytes = realscript::bytecode::encodeModule(compileFixture());
    writeU32(bytes, 20, 0);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::decodeModule(bytes, decoded, diagnostics),
            "overlapping section must fail decoding");
    require(hasDiagnostic(diagnostics, "RS5002"),
            "overlapping section must produce RS5002");
}

void testDecoderRejectsOversizedCount() {
    auto bytes = realscript::bytecode::encodeModule(compileFixture());
    const auto stringSectionOffset = readU32(bytes, 20);
    writeU32(bytes, stringSectionOffset, 0xffffffffu);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::decodeModule(bytes, decoded, diagnostics),
            "oversized string count must fail decoding");
    require(hasDiagnostic(diagnostics, "RS5003"),
            "oversized string count must produce RS5003");
}

void testVerifierRejectsInvalidRegister() {
    auto module = compileFixture();
    auto& instruction = module.functions.front().blocks.front().instructions.front();
    instruction.result = 9999;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::verifyModule(module, diagnostics),
            "invalid register must fail bytecode verification");
    require(hasDiagnostic(diagnostics, "RS5116"),
            "invalid definition must produce RS5116");
}

void testVerifierRejectsInvalidCallReference() {
    auto module = compileFixture();
    bool changed = false;
    for (auto& function : module.functions) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.opcode == realscript::bytecode::Opcode::Call) {
                    instruction.index = 9999;
                    changed = true;
                    break;
                }
            }
            if (changed) break;
        }
        if (changed) break;
    }
    require(changed, "fixture did not contain a call instruction");
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::verifyModule(module, diagnostics),
            "invalid call reference must fail verification");
    require(hasDiagnostic(diagnostics, "RS5136"),
            "invalid call reference must produce RS5136");
}

void testVerifierRejectsBranchArguments() {
    auto module = compileFixture();
    bool changed = false;
    for (auto& function : module.functions) {
        for (auto& block : function.blocks) {
            if (block.terminator.kind == realscript::bytecode::TerminatorKind::Branch &&
                (!block.terminator.arguments.empty() ||
                 !block.terminator.falseArguments.empty())) {
                block.terminator.arguments.clear();
                block.terminator.falseArguments.clear();
                changed = true;
                break;
            }
        }
        if (changed) break;
    }
    require(changed, "fixture did not contain an argument-carrying branch");
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::verifyModule(module, diagnostics),
            "invalid branch arguments must fail verification");
    require(hasDiagnostic(diagnostics, "RS5125"),
            "invalid branch arguments must produce RS5125");
}

void testEncodingIgnoresSourceInputOrder() {
    const std::vector<realscript::compiler::SourceFile> forward = {
        {"a.rs", "module Stable; int first() { return second(); }"},
        {"b.rs", "module Stable; int second() { return 2; }"},
    };
    const std::vector<realscript::compiler::SourceFile> reverse = {
        forward[1],
        forward[0],
    };
    const auto first = realscript::bytecode::encodeModule(
        compileBytecode(forward));
    const auto second = realscript::bytecode::encodeModule(
        compileBytecode(reverse));
    require(first == second, "source input order changed encoded bytecode");
}

void testSnapshots(bool writeSnapshots) {
    const std::string root = REALSCRIPT_SOURCE_DIR;
    const auto module = compileFixture();
    const auto disassembly = realscript::bytecode::disassembleModule(module);
    const auto hex = realscript::bytecode::bytesToHex(
        realscript::bytecode::encodeModule(module));
    const auto disassemblyPath =
        root + "/tests/snapshots/phase2a.bytecode.txt";
    const auto hexPath = root + "/tests/snapshots/phase2a.rsbc.hex.txt";

    if (writeSnapshots) {
        writeFile(disassemblyPath, disassembly);
        writeFile(hexPath, hex);
        return;
    }

    require(
        normalizeLineEndings(readFile(disassemblyPath)) ==
            normalizeLineEndings(disassembly),
        "bytecode disassembly snapshot changed");
    require(
        normalizeLineEndings(readFile(hexPath)) == normalizeLineEndings(hex),
        "bytecode binary snapshot changed");
}

} // namespace

int main(int argc, char** argv) {
    const bool writeSnapshots = argc == 2 &&
        std::string(argv[1]) == "--write-snapshots";
    if (writeSnapshots) {
        testSnapshots(true);
        std::cout << "wrote Phase 2A snapshots\n";
        return 0;
    }

    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };

    run("Bytecode lowering and verifier", testLoweringAndVerification);
    run("Canonical encode/decode", testRoundTripIsCanonical);
    run("Decoder bad magic", testDecoderRejectsBadMagic);
    run("Decoder version", testDecoderRejectsUnsupportedVersion);
    run("Decoder truncation", testDecoderRejectsTruncation);
    run("Decoder section overlap", testDecoderRejectsSectionOverlap);
    run("Decoder oversized count", testDecoderRejectsOversizedCount);
    run("Verifier invalid register", testVerifierRejectsInvalidRegister);
    run("Verifier invalid call", testVerifierRejectsInvalidCallReference);
    run("Verifier branch arguments", testVerifierRejectsBranchArguments);
    run("Deterministic source order", testEncodingIgnoresSourceInputOrder);
    run("Bytecode snapshots", [&] { testSnapshots(false); });

    return failures == 0 ? 0 : 1;
}
