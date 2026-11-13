#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DapServer.h"
#include "realscript/game/Gameplay.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/plugin/ModHost.h"
#include "realscript/reflection/Reflection.h"
#include "realscript/text/Text.h"
#include "realscript/tooling/LspServer.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
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

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FAILURE: " << message << "\n";
        std::exit(1);
    }
}

std::shared_ptr<const runtime::ProgramImage> compileAndLink(
    const std::vector<compiler::SourceFile>& sources) {
    compiler::Compilation comp(sources);
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        std::ostringstream oss;
        for (const auto& d : build.diagnostics.items()) {
            oss << d.code << ": " << d.message << "\n";
        }
        require(false, "Compilation failed:\n" + oss.str());
    }
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bc;
    bc.reserve(build.modules.size());
    for (const auto& m : build.modules) {
        bc.push_back(lowerer.lower(m));
    }
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bc), linkErr);
    require(linked.has_value(), "Link failed: " + linkErr.message);
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

#if defined(_WIN32)
using TestSocket = SOCKET;
constexpr TestSocket kInvalidTestSocket = INVALID_SOCKET;
void initTestSockets() {
    static const bool sInit = []() {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)sInit;
}
void closeTestSocket(TestSocket s) {
    if (s != INVALID_SOCKET) {
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
    }
}
#else
using TestSocket = int;
constexpr TestSocket kInvalidTestSocket = -1;
void initTestSockets() {}
void closeTestSocket(TestSocket s) {
    if (s >= 0) {
        ::shutdown(s, SHUT_RDWR);
        ::close(s);
    }
}
#endif

class ClientSocketBuf final : public std::streambuf {
public:
    explicit ClientSocketBuf(TestSocket sock) : sock_(sock) {
        setg(inBuf_, inBuf_, inBuf_);
        setp(outBuf_, outBuf_ + sizeof(outBuf_));
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
#if defined(_WIN32)
        const int n = ::recv(sock_, inBuf_, static_cast<int>(sizeof(inBuf_)), 0);
#else
        const auto n = ::recv(sock_, inBuf_, sizeof(inBuf_), 0);
#endif
        if (n <= 0) return traits_type::eof();
        setg(inBuf_, inBuf_, inBuf_ + n);
        return traits_type::to_int_type(*gptr());
    }

    int overflow(int_type ch) override {
        if (flushOut() != 0) return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }

    int sync() override {
        return flushOut();
    }

private:
    int flushOut() {
        const std::ptrdiff_t total = pptr() - pbase();
        std::ptrdiff_t sent = 0;
        while (sent < total) {
#if defined(_WIN32)
            const int n = ::send(sock_, outBuf_ + sent, static_cast<int>(total - sent), 0);
#else
            const auto n = ::send(sock_, outBuf_ + sent, static_cast<std::size_t>(total - sent), 0);
#endif
            if (n <= 0) return -1;
            sent += n;
        }
        pbump(static_cast<int>(-total));
        return 0;
    }

    TestSocket sock_;
    char inBuf_[4096]{};
    char outBuf_[4096]{};
};

tooling::Json readNextDapMessage(std::istream& stream) {
    std::string raw;
    require(tooling::readProtocolMessage(stream, raw), "failed to read DAP protocol message");
    std::string parseErr;
    auto parsed = tooling::Json::parse(raw, parseErr);
    require(parsed.has_value(), "DAP JSON parse error: " + parseErr);
    return std::move(*parsed);
}

// ============================================================================
// 1. AUD-01: LSP UTF-16 <-> UTF-8 Column Mapping, Incremental CJK Edits & Negotiation
// ============================================================================
void testLspUtf16CjkEmojiAndEncodingNegotiation() {
    std::cout << "[1/4] Testing AUD-01: LSP UTF-16 <-> UTF-8 CJK/Emoji columns, incremental edits & encoding negotiation...\n";

    // Unit-level UTF-8 <-> UTF-16 conversion including 3-byte CJK and 4-byte surrogate-pair emoji
    {
        // "中文🎮ab": '中'(3B,1u16) + '文'(3B,1u16) + '🎮'(4B,2u16) + 'a'(1B,1u16) + 'b'(1B,1u16)
        const std::string line = "中文\xF0\x9F\x8E\xAE" "ab";
        require(text::utf8ColumnToUtf16Column(line, 0) == 0, "u8->u16 col 0 mismatch");
        require(text::utf8ColumnToUtf16Column(line, 3) == 1, "u8->u16 after 1st CJK mismatch");
        require(text::utf8ColumnToUtf16Column(line, 6) == 2, "u8->u16 after 2nd CJK mismatch");
        require(text::utf8ColumnToUtf16Column(line, 10) == 4, "u8->u16 after 4-byte emoji mismatch");
        require(text::utf8ColumnToUtf16Column(line, 11) == 5, "u8->u16 after 'a' mismatch");

        require(text::utf16ColumnToUtf8Column(line, 0) == 0, "u16->u8 col 0 mismatch");
        require(text::utf16ColumnToUtf8Column(line, 1) == 3, "u16->u8 col 1 mismatch");
        require(text::utf16ColumnToUtf8Column(line, 2) == 6, "u16->u8 col 2 mismatch");
        require(text::utf16ColumnToUtf8Column(line, 3) == 6, "u16->u8 mid-surrogate must clamp cleanly to codepoint start");
        require(text::utf16ColumnToUtf8Column(line, 4) == 10, "u16->u8 after surrogate pair mismatch");
        require(text::utf16ColumnToUtf8Column(line, 5) == 11, "u16->u8 after 'a' mismatch");
    }

    // Default UTF-16 LSP session: Hover, Completion, SemanticTokens & Multi-Edit didChange on CJK lines
    {
        tooling::LspServer lsp;
        const auto initResp = lsp.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "initialize"},
            {"params", tooling::Json::Object{}},
        });
        const auto* encCap = initResp.find("result")->find("capabilities")->find("positionEncoding");
        require(encCap && encCap->stringValue() == "utf-16",
            "Default LSP initialize must advertise positionEncoding = 'utf-16'");
        require(lsp.positionEncoding() == tooling::LspServer::PositionEncoding::Utf16,
            "Default LspServer positionEncoding() must be Utf16");

        const std::string code =
            "module Demo;\n"
            "class Hero { public int hp; }\n"
            "int test(Hero heroArg) {\n"
            "    string s = \"角色生命\"; return heroArg.hp;\n"
            "}\n";
        // Line 3: `    string s = "角色生命"; return heroArg.hp;`
        // UTF-16 offsets: `    string s = "` (16) + `角色生命` (4) + `"; return ` (10) => `heroArg` is [30, 37), `.` is 37, `hp` is [38, 40)
        (void)lsp.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"method", "textDocument/didOpen"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{
                    {"uri", "file:///cjk_lsp.rscript"},
                    {"languageId", "realscript"},
                    {"version", 1},
                    {"text", code},
                }},
            }},
        });

        const auto hoverResp = lsp.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 2},
            {"method", "textDocument/hover"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{{"uri", "file:///cjk_lsp.rscript"}}},
                {"position", tooling::Json::Object{{"line", 3}, {"character", 31}}},
            }},
        });
        const auto* resObj = hoverResp.find("result");
        require(resObj && resObj->isObject(), "Hover at UTF-16 character 31 after CJK literal must resolve `heroArg`");
        const std::string hoverText = resObj->find("contents")->find("value")->stringValue();
        require(hoverText.find("heroArg") != std::string::npos,
            "Hover contents must describe parameter `heroArg`, got: " + hoverText);
        // Outgoing range on line 3 or declaration line 2 must also be UTF-16
        const auto compMsg = lsp.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "textDocument/completion"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{{"uri", "file:///cjk_lsp.rscript"}}},
                {"position", tooling::Json::Object{{"line", 3}, {"character", 38}}},
            }},
        });
        const auto* compResp = compMsg.find("result");
        require(compResp && compResp->isArray(), "Dot-completion after `heroArg.` on CJK line must return array");
        bool sawHp = false;
        for (const auto& item : compResp->arrayValue()) {
            if (item.find("label") && item.find("label")->stringValue() == "hp") {
                sawHp = true;
                break;
            }
        }
        require(sawHp, "Dot-completion at UTF-16 column 38 got: " + compResp->dump());

        // Semantic tokens: string literal `"角色生命"` is 6 UTF-16 code units (quotes + 4 CJK chars), NOT 14 UTF-8 bytes!
        const auto semResp = lsp.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 4},
            {"method", "textDocument/semanticTokens/full"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{{"uri", "file:///cjk_lsp.rscript"}}},
            }},
        });
        const auto& semData = semResp.find("result")->find("data")->arrayValue();
        bool foundCjkStringToken = false;
        std::size_t curLine = 0;
        std::size_t curChar = 0;
        for (std::size_t i = 0; i + 4 < semData.size(); i += 5) {
            const auto dLine = static_cast<std::size_t>(semData[i].integerValue());
            const auto dStart = static_cast<std::size_t>(semData[i + 1].integerValue());
            const auto len = static_cast<std::size_t>(semData[i + 2].integerValue());
            const auto tokType = semData[i + 3].integerValue();
            curLine += dLine;
            curChar = (dLine == 0) ? (curChar + dStart) : dStart;
            if (curLine == 3 && tokType == 9) { // 9 = string
                require(curChar == 15 && len == 6,
                    "Semantic token for '\"角色生命\"' must have UTF-16 start=15, length=6; got start=" +
                        std::to_string(curChar) + ", len=" + std::to_string(len));
                foundCjkStringToken = true;
            }
        }
        require(foundCjkStringToken, "Expected string semantic token on line 3");

        // Batched incremental `didChange` with UTF-16 columns after CJK text:
        // Replace `heroArg.hp` [30, 40) with `heroArg.hp + 7` AND in same batch replace `"角色生命"` [15, 21) with `"护盾"`
        (void)lsp.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"method", "textDocument/didChange"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{
                    {"uri", "file:///cjk_lsp.rscript"},
                    {"version", 2},
                }},
                {"contentChanges", tooling::Json::Array{
                    tooling::Json::Object{
                        {"range", tooling::Json::Object{
                            {"start", tooling::Json::Object{{"line", 3}, {"character", 30}}},
                            {"end", tooling::Json::Object{{"line", 3}, {"character", 40}}},
                        }},
                        {"text", "heroArg.hp + 7"},
                    },
                    tooling::Json::Object{
                        {"range", tooling::Json::Object{
                            {"start", tooling::Json::Object{{"line", 3}, {"character", 15}}},
                            {"end", tooling::Json::Object{{"line", 3}, {"character", 21}}},
                        }},
                        {"text", "\"护盾\""},
                    },
                }},
            }},
        });
        const std::string updated = lsp.service().documentContent("/cjk_lsp.rscript");
        require(
            updated.find("string s = \"护盾\"; return heroArg.hp + 7;") != std::string::npos,
            "Batched UTF-16 incremental didChange produced unexpected text:\n" + updated);
        require(lsp.service().diagnostics("/cjk_lsp.rscript").empty(),
            "Edited CJK document must compile cleanly with zero diagnostics");
    }

    // Client negotiating UTF-8 (`positionEncodings: ["utf-8", "utf-16"]`)
    {
        tooling::LspServer lspUtf8;
        const auto initResp = lspUtf8.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 10},
            {"method", "initialize"},
            {"params", tooling::Json::Object{
                {"capabilities", tooling::Json::Object{
                    {"general", tooling::Json::Object{
                        {"positionEncodings", tooling::Json::Array{"utf-8", "utf-16"}},
                    }},
                }},
            }},
        });
        const auto* encCap = initResp.find("result")->find("capabilities")->find("positionEncoding");
        require(encCap && encCap->stringValue() == "utf-8",
            "Client preferring utf-8 must negotiate positionEncoding = 'utf-8'");
        require(lspUtf8.positionEncoding() == tooling::LspServer::PositionEncoding::Utf8,
            "Negotiated LspServer positionEncoding() must be Utf8");
    }

    // 1b) AUD-01 extra regressions: inlayHints 0-based UTF-16/UTF-8, multi-line semanticTokens, standalone '\r', & disk read guard
    {
        // Standalone '\r' inside '\n'-delimited text must match compiler lineStarts ('\n'-indexed)
        const std::string mixedEol = "alpha\rbeta\ngamma\r\ndelta";
        require(text::lineContentAt(mixedEol, 0) == "alpha\rbeta", "line 0 standalone \\r mismatch");
        require(text::lineContentAt(mixedEol, 1) == "gamma", "line 1 CRLF strip mismatch");
        require(text::lineContentAt(mixedEol, 2) == "delta", "line 2 EOF mismatch");

        // Non-.rscript file must not be read off disk by LspServer for unopened URIs
        namespace fs = std::filesystem;
        const fs::path secretTxt = fs::temp_directory_path() / "realscript_p1_unopened_secret.txt";
        {
            std::ofstream out(secretTxt, std::ios::binary | std::ios::trunc);
            out << "CONFIDENTIAL_NON_RSCRIPT";
        }
        const std::string secretUri = tooling::pathToUri(secretTxt.string());
        tooling::LspServer lspHints;
        (void)lspHints.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 20},
            {"method", "initialize"},
            {"params", tooling::Json::Object{}},
        });
        const auto txtHover = lspHints.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 21},
            {"method", "textDocument/hover"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{{"uri", secretUri}}},
                {"position", tooling::Json::Object{{"line", 0}, {"character", 2}}},
            }},
        });
        std::error_code rmEc;
        fs::remove(secretTxt, rmEc);
        require(txtHover.find("result") && txtHover.find("result")->isNull(),
            "Unopened non-.rscript file must not be parsed or exposed via hover");

        // InlayHints + Multi-line string SemanticTokens with UTF-16 CJK text on line 0 / line 2
        const std::string hintCode =
            "module HintMod;\n"
            "int addOne(int valueArg) { return valueArg + 1; }\n"
            "int runHint() {\n"
            "    string banner = $\"第一行{1 +\n2}第二行\";\n"
            "    var inferred = addOne(41);\n"
            "    return inferred;\n"
            "}\n";
        (void)lspHints.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"method", "textDocument/didOpen"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{
                    {"uri", "file:///hint_utf16.rscript"},
                    {"languageId", "realscript"},
                    {"version", 1},
                    {"text", hintCode},
                }},
            }},
        });

        // Verify multi-line string token is split into single-line semantic tokens without overflow
        const auto semResp = lspHints.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 22},
            {"method", "textDocument/semanticTokens/full"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{{"uri", "file:///hint_utf16.rscript"}}},
            }},
        });
        const auto& semArr = semResp.find("result")->find("data")->arrayValue();
        std::size_t line = 0;
        std::size_t ch = 0;
        bool sawLine3String = false;
        bool sawLine4String = false;
        for (std::size_t i = 0; i + 4 < semArr.size(); i += 5) {
            const auto dL = static_cast<std::size_t>(semArr[i].integerValue());
            const auto dS = static_cast<std::size_t>(semArr[i + 1].integerValue());
            const auto len = static_cast<std::size_t>(semArr[i + 2].integerValue());
            const auto ty = semArr[i + 3].integerValue();
            line += dL;
            ch = (dL == 0) ? (ch + dS) : dS;
            if (line == 3 && ty == 9) {
                require(ch == 20 && len == 9, // `$"第一行{1 +` -> $"(2) + 3 CJK + {1 +(4) = 9 UTF-16 units
                    "Line 3 segment of multi-line string expected start=20, len=9, got " +
                        std::to_string(ch) + "/" + std::to_string(len));
                sawLine3String = true;
            } else if (line == 4 && ty == 9) {
                require(ch == 0 && len == 6, // `2}第二行"` -> 2}(2) + 3 CJK + "(1) = 6 UTF-16 units
                    "Line 4 segment of multi-line string expected start=0, len=6, got " +
                        std::to_string(ch) + "/" + std::to_string(len));
                sawLine4String = true;
            }
        }
        require(sawLine3String && sawLine4String,
            "Multi-line string literal must emit per-line semantic tokens across lines 3 and 4");

        // Line 5: `    var inferred = addOne(41);`
        // `inferred` ends at col 16, `41` starts at col 26
        const auto hintResp = lspHints.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 23},
            {"method", "textDocument/inlayHint"},
            {"params", tooling::Json::Object{
                {"textDocument", tooling::Json::Object{{"uri", "file:///hint_utf16.rscript"}}},
            }},
        });
        const auto* hintArr = hintResp.find("result");
        require(hintArr && hintArr->isArray() && hintArr->arrayValue().size() >= 2,
            "Expected var type & parameter inlay hints");
        bool sawTypeHintLine5 = false;
        bool sawParamHintLine5 = false;
        for (const auto& hItem : hintArr->arrayValue()) {
            const auto hLine = hItem.find("position")->find("line")->integerValue();
            const auto hChar = hItem.find("position")->find("character")->integerValue();
            const auto hLabel = hItem.find("label")->stringValue();
            if (hLabel == ": int" && hLine == 5 && hChar == 16) sawTypeHintLine5 = true;
            if (hLabel == "valueArg:" && hLine == 5 && hChar == 26) sawParamHintLine5 = true;
        }
        require(sawTypeHintLine5 && sawParamHintLine5,
            "InlayHint positions must be exact 0-based coordinates on line 5 (no off-by-one shift): " +
                hintResp.dump());
    }
}

// ============================================================================
// 2. AUD-02: Encapsulation Enforcement in `RealScript.Reflect` & `ReflectionRegistry`
// ============================================================================
void testReflectionNonPublicEncapsulationEnforcement() {
    std::cout << "[2/4] Testing AUD-02: Script & default C++ reflection non-public member encapsulation...\n";

    const std::string vaultLib = R"RS(
module VaultLib;

public class BankVault {
    [SecretFieldAttr("pin-tag")]
    private int secretPin = 4321;
    internal string auditToken = "internal_tok";
    public int publicBalance = 100;
    public int Shield { get; } = 50;
    [SecretPropAttr("prop-tag")]
    private int SecretProp { get; set; } = 25;

    [SecretMethodAttr("method-tag")]
    private int ComputeMasterKey() { return secretPin * 2; }
    public int ReadPinForTest() { return secretPin; }
    public int ReadShield() { return Shield; }
}

[SecretTypeAttr("hidden-type")]
private class HiddenVault {
    public int exposedLooking = 42;
    public int Ping() { return exposedLooking; }
    public static int StaticPing() { return 99; }
}

[SecretFnAttr("hidden-fn")]
private int secretModuleFn(int x) { return x + 100; }

[PublicFnAttr("pub-fn")]
public int publicModuleFn(int x) { return x + 1; }
)RS";

    const std::string modAttemptOverwrite = R"RS(
module MaliciousMod;
import VaultLib;
import RealScript.Reflect;

public int countPublicFields() {
    return Reflect.GetFieldCount("VaultLib::BankVault");
}

public string firstPublicFieldName() {
    return Reflect.GetFieldName("VaultLib::BankVault", 0);
}

public int countPublicMethods() {
    return Reflect.GetMethodCount("VaultLib::BankVault");
}

public string firstPublicMethodName() {
    return Reflect.GetMethodName("VaultLib::BankVault", 0);
}

public bool hasHiddenType() {
    return Reflect.HasType("VaultLib::HiddenVault");
}

public int countHiddenTypeFields() {
    return Reflect.GetFieldCount("VaultLib::HiddenVault");
}

public bool hasPrivateFieldAttr() {
    return Reflect.HasAttribute("VaultLib::BankVault::field:secretPin", "SecretFieldAttr") ||
           Reflect.HasAttribute("VaultLib::BankVault.secretPin", "SecretFieldAttr");
}

public bool hasHiddenTypeAttr() {
    return Reflect.HasAttribute("VaultLib::HiddenVault", "SecretTypeAttr");
}

public bool hasPrivatePropAttr() {
    return Reflect.HasAttribute("VaultLib::BankVault::property:SecretProp", "SecretPropAttr");
}

public bool hasPrivateModuleFnAttr() {
    return Reflect.HasAttribute("VaultLib::secretModuleFn", "SecretFnAttr") ||
           Reflect.HasAttribute("VaultLib::function:secretModuleFn#1", "SecretFnAttr");
}

public bool hasPublicModuleFnAttr() {
    return Reflect.HasAttribute("VaultLib::function:publicModuleFn#1", "PublicFnAttr");
}

public int overwritePublic() {
    var v = new BankVault();
    Reflect.SetIntField(v, "publicBalance", 250);
    return Reflect.GetIntField(v, "publicBalance");
}

public int attemptReadPrivate() {
    var v = new BankVault();
    return Reflect.GetIntField(v, "secretPin");
}

public int attemptWritePrivate() {
    var v = new BankVault();
    Reflect.SetIntField(v, "secretPin", 999999);
    return v.ReadPinForTest();
}

public int attemptWritePrivateAutoProp() {
    var v = new BankVault();
    Reflect.SetIntField(v, "SecretProp", 777);
    return v.ReadShield();
}

public int attemptWriteRawSyntheticBacking() {
    var v = new BankVault();
    Reflect.SetIntField(v, "$Shield", 888);
    return v.ReadShield();
}
)RS";

    auto program = compileAndLink({
        reflection::exportReflectDeclarationStub(),
        {"vault_lib.rscript", vaultLib},
        {"malicious_mod.rscript", modAttemptOverwrite},
    });
    runtime::EngineRuntime engine(program);
    auto refl = std::make_shared<reflection::ReflectionRegistry>(engine);
    auto bindings = std::make_shared<runtime::BindingRegistry>();
    reflection::registerReflectBindings(*bindings, refl, "reflection", engine.heap());
    engine.setBindings(bindings);

    runtime::ExecutionOptions opts;
    opts.enforceCapabilities = true;
    opts.grantedCapabilities = {"reflection"};

    // Public members remain accessible and enumerable
    const auto fCountRes = engine.invoke("MaliciousMod::countPublicFields", {}, opts);
    require(fCountRes.succeeded && std::get<std::int64_t>(fCountRes.value) == 1,
        "Reflect.GetFieldCount must only count public non-backing fields (expected 1: publicBalance)");
    const auto fNameRes = engine.invoke("MaliciousMod::firstPublicFieldName", {}, opts);
    require(fNameRes.succeeded, "Reflect.GetFieldName(0) on public field must succeed");
    const auto pubRwRes = engine.invoke("MaliciousMod::overwritePublic", {}, opts);
    require(pubRwRes.succeeded && std::get<std::int64_t>(pubRwRes.value) == 250,
        "Public field read/write via RealScript.Reflect must succeed");

    // Non-public fields, private-setter auto-properties, and synthetic `$Prop` slots MUST fail with CapabilityDenied
    for (const char* entry : {
             "MaliciousMod::attemptReadPrivate",
             "MaliciousMod::attemptWritePrivate",
             "MaliciousMod::attemptWritePrivateAutoProp",
             "MaliciousMod::attemptWriteRawSyntheticBacking"}) {
        const auto res = engine.invoke(entry, {}, opts);
        require(!res.succeeded && res.error.code == runtime::ErrorCode::CapabilityDenied,
            std::string(entry) + " must fail with CapabilityDenied, got succeeded=" +
                std::to_string(res.succeeded) + " msg=" + res.error.message);
    }

    // C++ host API: default allowNonPublic=false rejects private field & method, while explicit allowNonPublic=true succeeds
    runtime::RuntimeError err;
    auto vaultObj = refl->instantiateObject(engine, "VaultLib::BankVault", {}, err);
    require(vaultObj.has_value(), "BankVault instantiate failed: " + err.message);
    auto root = engine.heap()->retain(*vaultObj);

    err = {};
    require(!refl->getField(engine, *vaultObj, "secretPin", err) &&
                err.code == runtime::ErrorCode::CapabilityDenied,
        "Default C++ ReflectionRegistry::getField must reject private field");
    err = {};
    auto allowedPin = refl->getField(engine, *vaultObj, "secretPin", err, {}, /*allowNonPublic=*/true);
    require(allowedPin.has_value() && std::get<std::int64_t>(*allowedPin) == 4321,
        "Explicit allowNonPublic=true on C++ ReflectionRegistry::getField must read private field");

    // Reading public getter of `{ get; private set; }` succeeds by default, writing fails by default
    err = {};
    auto pubGetterVal = refl->getField(engine, *vaultObj, "Shield", err);
    require(pubGetterVal.has_value() && std::get<std::int64_t>(*pubGetterVal) == 50,
        "Public getter property read via getField must succeed");
    err = {};
    require(!refl->setField(engine, *vaultObj, "Shield", runtime::Value{std::int64_t{99}}, err) &&
                err.code == runtime::ErrorCode::InvalidArguments,
        "Get-only property write via setField must fail cleanly");
    err = {};
    require(!refl->setField(engine, *vaultObj, "SecretProp", runtime::Value{std::int64_t{99}}, err) &&
                err.code == runtime::ErrorCode::CapabilityDenied,
        "Private auto-property write via setField must fail with CapabilityDenied");

    // Private method invocation blocked unless allowNonPublic=true
    const auto deniedPrivMethod = refl->invokeMethod(engine, *vaultObj, "ComputeMasterKey", {});
    require(!deniedPrivMethod.succeeded && deniedPrivMethod.error.code == runtime::ErrorCode::CapabilityDenied,
        "Default invokeMethod must reject private method");
    const auto allowedPrivMethod = refl->invokeMethod(
        engine, *vaultObj, "ComputeMasterKey", {}, {}, /*allowNonPublic=*/true);
    require(allowedPrivMethod.succeeded && std::get<std::int64_t>(allowedPrivMethod.value) == 8642,
        "invokeMethod with allowNonPublic=true must invoke private method");

    // Non-public types, property accessors in GetMethodCount, module free functions, attributes & bytecode round-trip
    const auto mCountRes = engine.invoke("MaliciousMod::countPublicMethods", {}, opts);
    require(mCountRes.succeeded && std::get<std::int64_t>(mCountRes.value) == 2,
        "Reflect.GetMethodCount must exclude private methods and property accessors (expected 2: ReadPinForTest, ReadShield)");
    const auto hiddenTypeRes = engine.invoke("MaliciousMod::hasHiddenType", {}, opts);
    require(hiddenTypeRes.succeeded && !std::get<bool>(hiddenTypeRes.value),
        "Reflect.HasType must hide private/internal types");
    const auto hiddenFieldCountRes = engine.invoke("MaliciousMod::countHiddenTypeFields", {}, opts);
    require(hiddenFieldCountRes.succeeded && std::get<std::int64_t>(hiddenFieldCountRes.value) == 0,
        "Reflect.GetFieldCount must return 0 for non-public types");
    for (const char* attrCheck : {
             "MaliciousMod::hasPrivateFieldAttr",
             "MaliciousMod::hasHiddenTypeAttr",
             "MaliciousMod::hasPrivatePropAttr",
             "MaliciousMod::hasPrivateModuleFnAttr"}) {
        const auto r = engine.invoke(attrCheck, {}, opts);
        require(r.succeeded && !std::get<bool>(r.value),
            std::string(attrCheck) + " must hide non-public target attributes from scripts");
    }
    const auto pubFnAttrRes = engine.invoke("MaliciousMod::hasPublicModuleFnAttr", {}, opts);
    require(pubFnAttrRes.succeeded && std::get<bool>(pubFnAttrRes.value),
        "Public module function attribute must remain visible");

    // C++ private type instantiation & invocation blocked by default, allowed when allowNonPublic=true
    err = {};
    require(!refl->instantiateObject(engine, "VaultLib::HiddenVault", {}, err).has_value() &&
                err.code == runtime::ErrorCode::CapabilityDenied,
        "instantiateObject must deny private types unless allowNonPublic=true");
    err = {};
    auto hiddenObj = refl->instantiateObject(
        engine, "VaultLib::HiddenVault", {}, err, true, {}, /*allowNonPublic=*/true);
    require(hiddenObj.has_value(), "instantiateObject with allowNonPublic=true on private type failed");
    auto hiddenRoot = engine.heap()->retain(*hiddenObj);
    const auto deniedHiddenMethod = refl->invokeMethod(engine, *hiddenObj, "Ping", {});
    require(!deniedHiddenMethod.succeeded && deniedHiddenMethod.error.code == runtime::ErrorCode::CapabilityDenied,
        "invokeMethod must deny methods on a private runtime type unless allowNonPublic=true");
    const auto deniedHiddenStatic = refl->invokeStaticMethod(engine, "VaultLib::HiddenVault", "StaticPing", {});
    require(!deniedHiddenStatic.succeeded && deniedHiddenStatic.error.code == runtime::ErrorCode::CapabilityDenied,
        "invokeStaticMethod must deny static methods on a private type unless allowNonPublic=true");
    const auto allowedHiddenStatic = refl->invokeStaticMethod(
        engine, "VaultLib::HiddenVault", "StaticPing", {}, {}, /*allowNonPublic=*/true);
    require(allowedHiddenStatic.succeeded && std::get<std::int64_t>(allowedHiddenStatic.value) == 99,
        "invokeStaticMethod with allowNonPublic=true must succeed on private type");

    // Non-public module free-function accessibility preserved across bytecode serialization round-trip!
    require(!refl->hasAttributeOnTarget("VaultLib::BankVault::field:secretPin", "SecretFieldAttr"),
        "C++ hasAttributeOnTarget must deny private field by default");
    require(refl->hasAttributeOnTarget("VaultLib::BankVault::field:secretPin", "SecretFieldAttr", /*allowNonPublic=*/true),
        "C++ hasAttributeOnTarget with allowNonPublic=true must find private field attribute");

    const auto deniedModFn = refl->invokeStaticMethod(
        engine, "VaultLib", "secretModuleFn", {runtime::Value{std::int64_t{5}}});
    require(!deniedModFn.succeeded && deniedModFn.error.code == runtime::ErrorCode::CapabilityDenied,
        "invokeStaticMethod must deny non-public module free function by default");

    compiler::Compilation vaultComp({{"vault_lib.rscript", vaultLib}});
    auto vaultBuild = vaultComp.build();
    require(!vaultBuild.diagnostics.hasErrors() && !vaultBuild.modules.empty(), "vault standalone build failed");
    bytecode::Lowerer bcLow;
    const auto encoded = bytecode::encodeModule(bcLow.lower(vaultBuild.modules.front()));
    bytecode::Module decodedMod;
    diagnostics::DiagnosticBag decodeDiag;
    require(bytecode::decodeModule(encoded, decodedMod, decodeDiag) && !decodeDiag.hasErrors(),
        "Vault bytecode decode failed");
    runtime::RuntimeError roundTripLinkErr;
    auto roundTripProg = runtime::ProgramImage::link({decodedMod}, roundTripLinkErr);
    require(roundTripProg.has_value(), "Round-trip link failed");
    runtime::EngineRuntime roundTripEngine(std::make_shared<const runtime::ProgramImage>(std::move(*roundTripProg)));
    reflection::ReflectionRegistry roundTripRefl(roundTripEngine);
    const auto roundTripDenied = roundTripRefl.invokeStaticMethod(
        roundTripEngine, "VaultLib", "secretModuleFn", {runtime::Value{std::int64_t{7}}});
    require(!roundTripDenied.succeeded && roundTripDenied.error.code == runtime::ErrorCode::CapabilityDenied,
        "Decoded bytecode must preserve non-public module function accessibility and deny default reflection call");
    const auto roundTripAllowed = roundTripRefl.invokeStaticMethod(
        roundTripEngine, "VaultLib", "secretModuleFn", {runtime::Value{std::int64_t{7}}}, {}, /*allowNonPublic=*/true);
    require(roundTripAllowed.succeeded && std::get<std::int64_t>(roundTripAllowed.value) == 107,
        "allowNonPublic=true on decoded non-public module function must succeed");
}

// ============================================================================
// 3. AUD-03: Static Constructor Budget/Capability Propagation & Uninitialized Guard
// ============================================================================
void testReflectionStaticConstructorBudgetAndInitGuard() {
    std::cout << "[3/4] Testing AUD-03: Reflection static constructor (.cctor) ExecutionOptions enforcement & heap guard...\n";

    const std::string src = R"RS(
module StaticGuardTest;

public class HeavyStatic {
    public static int Computed = 0;
    public int tag = 1;

    static HeavyStatic() {
        int acc = 0;
        for (int i = 0; i < 4000; i = i + 1) {
            acc = acc + 1;
        }
        Computed = acc;
    }
}
)RS";

    auto program = compileAndLink({{"static_guard.rscript", src}});

    // 3a) Pure-heap getField/setField/getStaticField without EngineRuntime before .cctor runs MUST NOT silently return 0
    {
        auto rawHeap = std::make_shared<runtime::ManagedHeap>();
        reflection::ReflectionRegistry refl(program, rawHeap);
        runtime::RuntimeError err;
        auto uninitObj = refl.instantiateUninitializedObject(*rawHeap, "StaticGuardTest::HeavyStatic", err);
        require(uninitObj.has_value(), "instantiateUninitializedObject failed");
        auto root = rawHeap->retain(*uninitObj);

        err = {};
        require(!refl.getField(*rawHeap, *uninitObj, "Computed", err).has_value() &&
                    err.code == runtime::ErrorCode::InvalidArguments,
            "Pure-heap getField on static field with uninitialized .cctor must fail instead of returning stale 0");

        err = {};
        require(!refl.getStaticField(*rawHeap, "StaticGuardTest::HeavyStatic", "Computed", err, nullptr).has_value() &&
                    err.code == runtime::ErrorCode::InvalidArguments,
            "getStaticField without EngineRuntime on uninitialized .cctor must fail cleanly");
    }

    // 3b) Tight instructionBudget passed to getStaticField MUST halt runaway .cctor cleanly WITHOUT poisoning guard!
    {
        runtime::EngineRuntime budgetedEngine(program);
        reflection::ReflectionRegistry refl(budgetedEngine);
        runtime::ExecutionOptions tightBudget;
        tightBudget.limits.instructionBudget = 80;

        runtime::RuntimeError err;
        auto blocked = refl.getStaticField(
            *budgetedEngine.heap(), "StaticGuardTest::HeavyStatic", "Computed", err, &budgetedEngine, tightBudget);
        require(!blocked.has_value() && err.code == runtime::ErrorCode::InstructionBudgetExceeded,
            "getStaticField must propagate ExecutionOptions instructionBudget into .cctor");

        // Retry on the SAME engine/heap with ample budget: .cctor must NOT be poisoned!
        runtime::ExecutionOptions ampleBudget;
        ampleBudget.limits.instructionBudget = 200'000;
        err = {};
        auto retried = refl.getStaticField(
            *budgetedEngine.heap(), "StaticGuardTest::HeavyStatic", "Computed", err, &budgetedEngine, ampleBudget);
        require(retried.has_value() && std::get<std::int64_t>(*retried) == 4000,
            "Failed .cctor must not poison guard; retry with ample budget must initialize Computed=4000");
    }

    // 3c) Base-to-derived .cctor ordering, inherited static field lookup, & heap mismatch guard
    {
        const std::string inheritSrc = R"RS(
module InheritCctor;

public class BaseCfg {
    public static int BaseSeed = 0;
    public static int LogOrder = 0;
    static BaseCfg() {
        BaseSeed = 11;
        LogOrder = 1;
    }
}

public class DerivedCfg : BaseCfg {
    public static int DerivedVal = 0;
    static DerivedCfg() {
        DerivedVal = BaseSeed * 10 + 2;
        LogOrder = LogOrder * 10 + 2;
    }
}
)RS";
        auto inheritProg = compileAndLink({{"inherit_cctor.rscript", inheritSrc}});
        runtime::EngineRuntime inheritEngine(inheritProg);
        reflection::ReflectionRegistry inheritRefl(inheritEngine);

        // Accessing inherited static field via DerivedCfg initializes BaseCfg cleanly
        runtime::RuntimeError err;
        auto baseSeed = inheritRefl.getStaticField(
            *inheritEngine.heap(), "InheritCctor::DerivedCfg", "BaseSeed", err, &inheritEngine);
        require(baseSeed.has_value() && std::get<std::int64_t>(*baseSeed) == 11,
            "Reading inherited static field on DerivedCfg must run BaseCfg .cctor and return 11");

        // Instantiating DerivedCfg runs BaseCfg (if needed) then DerivedCfg in base-to-derived order
        err = {};
        auto drvObj = inheritRefl.instantiateObject(inheritEngine, "InheritCctor::DerivedCfg", {}, err);
        require(drvObj.has_value(), "instantiateObject DerivedCfg failed: " + err.message);
        err = {};
        auto drvVal = inheritRefl.getStaticField(
            *inheritEngine.heap(), "InheritCctor::DerivedCfg", "DerivedVal", err, nullptr);
        require(drvVal.has_value() && std::get<std::int64_t>(*drvVal) == 112,
            "DerivedCfg .cctor must see BaseSeed==11 and compute 112");

        // Passing mismatched foreign heap + runtime must fail instead of silently mutating runtime heap
        auto foreignHeap = std::make_shared<runtime::ManagedHeap>();
        err = {};
        require(!inheritRefl.getStaticField(*foreignHeap, "InheritCctor::BaseCfg", "BaseSeed", err, &inheritEngine).has_value() &&
                    err.code == runtime::ErrorCode::InvalidObjectReference,
            "Heap mismatch in getStaticField must fail with InvalidObjectReference");
    }

    // 3d) Sufficient budget initializes .cctor cleanly, after which pure-heap reads also succeed
    {
        runtime::EngineRuntime okEngine(program);
        reflection::ReflectionRegistry refl(okEngine);
        runtime::ExecutionOptions ampleBudget;
        ampleBudget.limits.instructionBudget = 200'000;

        runtime::RuntimeError err;
        auto val = refl.getStaticField(
            *okEngine.heap(), "StaticGuardTest::HeavyStatic", "Computed", err, &okEngine, ampleBudget);
        require(val.has_value() && std::get<std::int64_t>(*val) == 4000,
            "getStaticField with ample budget must run .cctor and return 4000");

        auto pureHeapRead = refl.getStaticField(
            *okEngine.heap(), "StaticGuardTest::HeavyStatic", "Computed", err, nullptr);
        require(pureHeapRead.has_value() && std::get<std::int64_t>(*pureHeapRead) == 4000,
            "After .cctor is initialized on heap, pure-heap getStaticField must succeed");
    }

    // 3e) Re-entrant .cctor calling own static helper must not infinitely recurse and must roll back on tight budget
    {
        const std::string reentrantSrc = R"RS(
module ReentrantCctorTest;

public class HelperInit {
    public static int Value = InitValue();
    private static int InitValue() {
        int acc = 0;
        for (int i = 0; i < 20; i = i + 1) {
            acc = acc + 2;
        }
        return acc + 2;
    }
    public static int ReadValue() {
        return Value;
    }
}
)RS";
        auto reentrantProg = compileAndLink({{"reentrant_cctor.rscript", reentrantSrc}});
        runtime::EngineRuntime rt(reentrantProg);

        runtime::ExecutionOptions tight;
        tight.limits.instructionBudget = 15;
        const auto aborted = rt.invoke("ReentrantCctorTest::HelperInit.ReadValue", {}, tight);
        require(!aborted.succeeded && aborted.error.code == runtime::ErrorCode::InstructionBudgetExceeded,
            "Tiny budget during .cctor must fail with InstructionBudgetExceeded");

        runtime::ExecutionOptions okOpts;
        okOpts.limits.instructionBudget = 200'000;
        const auto recovered = rt.invoke("ReentrantCctorTest::HelperInit.ReadValue", {}, okOpts);
        require(recovered.succeeded && std::get<std::int64_t>(recovered.value) == 42,
            "Aborted re-entrant .cctor must roll back guard cleanly and return 42 on retry");
    }
}

// ============================================================================
// 4. AUD-04: EmbeddedDapServer Live Hot-Reload Program Rebind & Empty-Address Loopback
// ============================================================================
void testEmbeddedDapHotReloadRebindAndLoopbackDefault() {
    std::cout << "[4/4] Testing AUD-04: EmbeddedDapServer live hot-reload active server sync & loopback bind default...\n";
    initTestSockets();

    const std::string v1Src = R"RS(
module Game.LiveDap;

class LiveActor {
    public int Tick(int x) {
        int r = x + 1;
        return r;
    }
}
)RS";

    const std::string v2Src = R"RS(
module Game.LiveDap;

public class NewlyAddedReloadBadge {
    public int level = 77;
}

class LiveActor {
    public NewlyAddedReloadBadge badge = new NewlyAddedReloadBadge();

    public int Tick(int x) {
        int r = x + badge.level;
        return r;
    }
}
)RS";

    game::GameApi api;
    game::GameScriptCompiler compiler(api);
    auto v1Compiled = compiler.compile({{"live_dap.rscript", v1Src}});
    require(v1Compiled.succeeded(), "v1 compile failed");

    game::ScriptRuntime rt(v1Compiled.program);
    game::SceneScriptRuntime scene(rt);
    require(scene.attachComponent(1, "Game.LiveDap::LiveActor"), "attach LiveActor failed");

    // Start EmbeddedDapServer with EMPTY bindAddress ("") -> must bind safely to 127.0.0.1 (INADDR_LOOPBACK), NOT 0.0.0.0!
    debug::EmbeddedDapServer embedded(rt.program(), rt.heap());
    require(embedded.start(0, ""), "EmbeddedDapServer::start(0, \"\") failed");
    const std::uint16_t port = embedded.boundPort();
    require(port > 0, "boundPort must be > 0");
    require(embedded.boundAddress() == "127.0.0.1",
        "EmbeddedDapServer empty bindAddress must resolve to loopback 127.0.0.1, got: " +
            embedded.boundAddress());

    // Verify another socket CAN bind specifically to 0.0.0.0? No, verify connecting to 127.0.0.1 succeeds AND inspect peer/sockname on client/bound!
    TestSocket sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(sock != kInvalidTestSocket, "client socket creation failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    require(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
        "connect to 127.0.0.1 loopback failed when bindAddress was empty");

    // Also verify a separate socket binding a non-loopback check: try binding a raw socket WITHOUT SO_REUSEADDR on 127.0.0.1:port -> MUST fail with EADDRINUSE because embedded bound 127.0.0.1:port!
    {
        TestSocket probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe != kInvalidTestSocket) {
            sockaddr_in dupLoopback{};
            dupLoopback.sin_family = AF_INET;
            dupLoopback.sin_port = htons(port);
            (void)::inet_pton(AF_INET, "127.0.0.1", &dupLoopback.sin_addr);
            require(::bind(probe, reinterpret_cast<sockaddr*>(&dupLoopback), sizeof(dupLoopback)) != 0,
                "127.0.0.1:port must already be exclusively occupied by EmbeddedDapServer");
            closeTestSocket(probe);
        }
    }

    ClientSocketBuf clientBuf(sock);
    std::iostream dapStream(&clientBuf);

    // Initialize + attach over live TCP socket BEFORE hot reload
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 1},
        {"type", "request"},
        {"command", "initialize"},
        {"arguments", tooling::Json::Object{}},
    });
    dapStream.flush();
    (void)readNextDapMessage(dapStream); // initialize response
    (void)readNextDapMessage(dapStream); // initialized event

    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 2},
        {"type", "request"},
        {"command", "attach"},
        {"arguments", tooling::Json::Object{{"stopOnEntry", false}}},
    });
    dapStream.flush();
    const auto attachResp = readNextDapMessage(dapStream);
    require(attachResp.find("success")->boolValue(), "DAP attach failed");

    // Perform Hot Reload to v2 (introducing `Game.LiveDap::NewlyAddedReloadBadge` which did NOT exist in v1!)
    // WHILE the TCP client remains continuously connected to `server.run(...)`!
    auto v2Compiled = compiler.compile({{"live_dap.rscript", v2Src}});
    require(v2Compiled.succeeded(), "v2 compile failed");
    const auto hrReport = scene.hotReload(v2Compiled.program);
    require(hrReport.errors.empty(), "SceneScriptRuntime::hotReload to v2 failed");
    embedded.rebindProgram(rt.program());

    // Allocate an instance of `NewlyAddedReloadBadge` on the live heap and retain it so heapSnapshot inspects it!
    const auto badgeTid = semantic::stableTypeId("Game.LiveDap::NewlyAddedReloadBadge");
    auto badgeObj = rt.heap()->allocateObject(
        badgeTid, {runtime::Value{std::int64_t{77}}}, {}, nullptr);
    require(badgeObj.has_value(), "allocateObject NewlyAddedReloadBadge failed");
    auto badgeRoot = rt.heap()->retain(*badgeObj);

    // Request `realscript/heapSnapshot` over the SAME pre-existing TCP connection!
    // Without AUD-04 fix, the live `DapServer` still held v1's `ProgramImage` and formatted `NewlyAddedReloadBadge` as generic `"Record"`!
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 3},
        {"type", "request"},
        {"command", "realscript/heapSnapshot"},
        {"arguments", tooling::Json::Object{{"diffBaseline", false}}},
    });
    dapStream.flush();
    const auto snapResp = readNextDapMessage(dapStream);
    require(snapResp.find("success")->boolValue(), "heapSnapshot over live TCP failed");
    const std::string snapDump = snapResp.dump();
    require(
        snapDump.find("NewlyAddedReloadBadge") != std::string::npos,
        "Active TCP DapServer failed to resolve newly hot-reloaded type 'NewlyAddedReloadBadge' in heapSnapshot:\n" +
            snapDump.substr(0, 600));

    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 4},
        {"type", "request"},
        {"command", "disconnect"},
        {"arguments", tooling::Json::Object{}},
    });
    dapStream.flush();
    (void)readNextDapMessage(dapStream);

    closeTestSocket(sock);
    embedded.stop();
    require(embedded.boundAddress().empty(), "boundAddress must clear after stop()");

    // Verify DebugSession::rebindProgram(nullptr) safely clears controller bindings without dangling
    embedded.rebindProgram(nullptr);
    require(embedded.session() && embedded.session()->program() == nullptr,
        "rebindProgram(nullptr) must clear DebugSession program cleanly");
    embedded.rebindProgram(rt.program());
    require(embedded.session()->program() == rt.program(),
        "rebindProgram after nullptr must rebind cleanly");
}

} // namespace

int main() {
    testLspUtf16CjkEmojiAndEncodingNegotiation();
    testReflectionNonPublicEncapsulationEnforcement();
    testReflectionStaticConstructorBudgetAndInitGuard();
    testEmbeddedDapHotReloadRebindAndLoopbackDefault();
    std::cout << "All P1 audit boundary regression tests passed!\n";
    return 0;
}
