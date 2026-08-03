#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    text = read(path)
    if new in text:
        return
    if old not in text:
        raise RuntimeError(f"anchor not found in {path}: {old[:160]!r}")
    write(path, text.replace(old, new, 1))


# Bytecode 0.6 is now the only in-memory and serialized Phase 18 format.
replace_once(
    "src/bytecode/BytecodeVerifier.cpp",
    "if (module.version.major != 0 || module.version.minor != 5)",
    "if (module.version.major != 0 || module.version.minor != 6)")
replace_once(
    "tests/phase4_tests.cpp",
    'require(module.version.minor == 5, "Phase 4 must use .rsbc 0.5");',
    'require(module.version.minor == 6, "Phase 18 must use .rsbc 0.6");')

# Language metadata is a hot-reload compatibility contract.
replace_once(
    "include/realscript/hot_reload/HotReload.h",
    "    FunctionSignatureChanged,\n};",
    "    FunctionSignatureChanged,\n    LanguageMetadataChanged,\n};")

metadata_helpers = r'''
bool sameLanguageMetadata(
    const compiler::LanguageModuleMetadata& left,
    const compiler::LanguageModuleMetadata& right) {
    if (left.attributes.size() != right.attributes.size() ||
        left.interfaces.size() != right.interfaces.size() ||
        left.genericInstantiations.size() !=
            right.genericInstantiations.size() ||
        left.sequences.size() != right.sequences.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.attributes.size(); ++index) {
        const auto& first = left.attributes[index];
        const auto& second = right.attributes[index];
        if (first.target != second.target || first.name != second.name ||
            first.arguments.size() != second.arguments.size()) {
            return false;
        }
        for (std::size_t argument = 0;
             argument < first.arguments.size(); ++argument) {
            if (first.arguments[argument].name !=
                    second.arguments[argument].name ||
                first.arguments[argument].value !=
                    second.arguments[argument].value) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < left.interfaces.size(); ++index) {
        if (left.interfaces[index].typeName !=
                right.interfaces[index].typeName ||
            left.interfaces[index].interfaces !=
                right.interfaces[index].interfaces) {
            return false;
        }
    }
    for (std::size_t index = 0;
         index < left.genericInstantiations.size(); ++index) {
        const auto& first = left.genericInstantiations[index];
        const auto& second = right.genericInstantiations[index];
        if (first.genericName != second.genericName ||
            first.arguments != second.arguments ||
            first.generatedName != second.generatedName) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.sequences.size(); ++index) {
        const auto& first = left.sequences[index];
        const auto& second = right.sequences[index];
        if (first.typeName != second.typeName ||
            first.name != second.name ||
            first.callbacks != second.callbacks) {
            return false;
        }
    }
    return true;
}
'''
replace_once(
    "src/hot_reload/HotReload.cpp",
    '''bool sameSignature(const bytecode::Function& left, const bytecode::Function& right) {
    return left.symbolId == right.symbolId && left.name == right.name &&
        left.returnType == right.returnType && left.returnTypeId == right.returnTypeId &&
        left.parameterTypes == right.parameterTypes &&
        left.parameterTypeIds == right.parameterTypeIds;
}
''',
    '''bool sameSignature(const bytecode::Function& left, const bytecode::Function& right) {
    return left.symbolId == right.symbolId && left.name == right.name &&
        left.returnType == right.returnType && left.returnTypeId == right.returnTypeId &&
        left.parameterTypes == right.parameterTypes &&
        left.parameterTypeIds == right.parameterTypeIds;
}
''' + metadata_helpers)

replace_once(
    "src/hot_reload/HotReload.cpp",
    '''    if (moduleSet(oldModules) != moduleSet(newModules)) {
        plan.issues.push_back({
            ReloadIssueKind::ModuleSetChanged,
            {},
            "hot reload cannot add or remove modules",
        });
    }

    const auto oldTypes = types(oldModules);
''',
    '''    if (moduleSet(oldModules) != moduleSet(newModules)) {
        plan.issues.push_back({
            ReloadIssueKind::ModuleSetChanged,
            {},
            "hot reload cannot add or remove modules",
        });
    }
    for (const auto& oldModule : oldModules) {
        const auto replacement = std::find_if(
            newModules.begin(), newModules.end(),
            [&](const bytecode::Module& module) {
                return module.name == oldModule.name;
            });
        if (replacement != newModules.end() &&
            !sameLanguageMetadata(
                oldModule.languageMetadata,
                replacement->languageMetadata)) {
            plan.issues.push_back({
                ReloadIssueKind::LanguageMetadataChanged,
                oldModule.name,
                "language metadata changed during body-only hot reload",
            });
        }
    }

    const auto oldTypes = types(oldModules);
''')
replace_once(
    "src/hot_reload/HotReload.cpp",
    '''    case ReloadIssueKind::FunctionSignatureChanged: return "function-signature-changed";
    }
''',
    '''    case ReloadIssueKind::FunctionSignatureChanged: return "function-signature-changed";
    case ReloadIssueKind::LanguageMetadataChanged: return "language-metadata-changed";
    }
''')

# LSP must expose only original-source declarations, never compiler-owned names.
replace_once(
    "tests/phase4_tests.cpp",
    '''    require(!service.documentSymbols("math.rs").empty(),
        "document symbols are missing");
    service.update("app.rs", "module Phase4.App; int main() { return missing; }", 2);
''',
    '''    require(!service.documentSymbols("math.rs").empty(),
        "document symbols are missing");

    service.open("native_tools.rs", R"(
module Phase4.NativeTools;
delegate void ChangedHandler(int amount);
class Counter
{
    int total;
    event ChangedHandler Changed;
    void Add(int amount) { total = total + amount; }
    int Run() { Changed += Add; Changed(3); return total; }
}
T Identity<T>(T value) { return value; }
int main()
{
    Counter counter = new Counter();
    return Identity<int>(counter.Run());
}
)");
    require(service.diagnostics("native_tools.rs").empty(),
        "native language tooling fixture produced diagnostics");
    const auto nativeSymbols = service.documentSymbols("native_tools.rs");
    require(!nativeSymbols.empty(),
        "native language document symbols are missing");
    require(std::all_of(
            nativeSymbols.begin(), nativeSymbols.end(),
            [](const auto& symbol) {
                return symbol.name.find("$event_") == std::string::npos &&
                    symbol.name.find("$sequence_") == std::string::npos &&
                    symbol.name.find("__int") == std::string::npos;
            }),
        "LSP exposed compiler-owned native language symbols");

    service.update("app.rs", "module Phase4.App; int main() { return missing; }", 2);
''')

# DAP entry frames must remain user-facing with native event/generic lowering.
replace_once(
    "tests/phase4_tests.cpp",
    '''    realscript::tooling::Json disconnect = realscript::tooling::Json::Object{
        {"seq", 4}, {"type", "request"}, {"command", "disconnect"},
    };
    (void)dap.handle(disconnect);
}

void testHotReload() {
''',
    '''    realscript::tooling::Json disconnect = realscript::tooling::Json::Object{
        {"seq", 4}, {"type", "request"}, {"command", "disconnect"},
    };
    (void)dap.handle(disconnect);

    auto nativeProgram = link(compile({{"native_dap.rs", R"(
module Phase4.NativeDap;
delegate void ChangedHandler(int amount);
class Counter
{
    int total;
    event ChangedHandler Changed;
    void Add(int amount) { total = total + amount; }
    int Run() { Changed += Add; Changed(3); return total; }
}
T Identity<T>(T value) { return value; }
int main()
{
    Counter counter = new Counter();
    return Identity<int>(counter.Run());
}
)"}}));
    realscript::debug::DapServer nativeDap(nativeProgram);
    require(nativeDap.handle(initialize).find("success")->boolValue(),
        "native DAP initialize failed");
    realscript::tooling::Json nativeLaunch = realscript::tooling::Json::Object{
        {"seq", 5}, {"type", "request"}, {"command", "launch"},
        {"arguments", realscript::tooling::Json::Object{
            {"function", "Phase4.NativeDap::main"}, {"stopOnEntry", true},
        }},
    };
    std::ostringstream nativeEvents;
    require(nativeDap.handle(nativeLaunch, &nativeEvents)
            .find("success")->boolValue(),
        "native DAP launch failed");
    realscript::tooling::Json nativeStackRequest =
        realscript::tooling::Json::Object{
            {"seq", 6}, {"type", "request"},
            {"command", "stackTrace"},
            {"arguments", realscript::tooling::Json::Object{{"threadId", 1}}},
        };
    const auto nativeStack = nativeDap.handle(nativeStackRequest);
    const auto& nativeFrames = nativeStack.find("body")
        ->find("stackFrames")->arrayValue();
    require(!nativeFrames.empty() && nativeFrames.front().find("name"),
        "native DAP stackTrace returned no frame name");
    const auto nativeFrameName =
        nativeFrames.front().find("name")->stringValue();
    require(nativeFrameName.find("main") != std::string::npos &&
            nativeFrameName.find("$event_") == std::string::npos &&
            nativeFrameName.find("__int") == std::string::npos,
        "DAP exposed a compiler-owned native language frame");
    (void)nativeDap.handle(disconnect);
}

void testHotReload() {
''')

# Metadata changes are not body-only hot reloads.
replace_once(
    "tests/phase4_tests.cpp",
    '''    const auto layoutPlan = realscript::hot_reload::prepare(
        *typeProgram, std::move(changedType));
    require(!layoutPlan.compatible && std::any_of(
            layoutPlan.issues.begin(), layoutPlan.issues.end(), [](const auto& issue) {
                return issue.kind == realscript::hot_reload::ReloadIssueKind::TypeLayoutChanged;
            }),
        "type-layout-changing hot reload was not rejected");
}
''',
    '''    const auto layoutPlan = realscript::hot_reload::prepare(
        *typeProgram, std::move(changedType));
    require(!layoutPlan.compatible && std::any_of(
            layoutPlan.issues.begin(), layoutPlan.issues.end(), [](const auto& issue) {
                return issue.kind == realscript::hot_reload::ReloadIssueKind::TypeLayoutChanged;
            }),
        "type-layout-changing hot reload was not rejected");

    auto metadataInitial = compile({{"metadata_reload.rs", R"(
module Phase4.MetadataReload;
[Replicated(channel = "state")]
class Item { int value; }
int read(Item item) { return item.value; }
)"}});
    auto metadataProgram = link(metadataInitial);
    auto metadataChanged = compile({{"metadata_reload.rs", R"(
module Phase4.MetadataReload;
[Replicated(channel = "effects")]
class Item { int value; }
int read(Item item) { return item.value; }
)"}});
    const auto metadataPlan = realscript::hot_reload::prepare(
        *metadataProgram, std::move(metadataChanged));
    require(!metadataPlan.compatible && std::any_of(
            metadataPlan.issues.begin(), metadataPlan.issues.end(),
            [](const auto& issue) {
                return issue.kind ==
                    realscript::hot_reload::ReloadIssueKind::LanguageMetadataChanged;
            }),
        "language-metadata-changing hot reload was not rejected");
}
''')

# Roadmap closure accurately describes the implemented bounded delegate profile.
replace_once(
    "docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md",
    "### 18B — delegates, lambdas, and events — pending\n\n- Native delegate/event declarations.\n- Lambda syntax trees and closure analysis.\n- Method-group conversion and deterministic event ordering.\n- First migration may retain bounded captures, but the AST and symbols must be native.\n",
    "### 18B — delegates, lambdas, and events — complete for the bounded profile\n\nImplemented and validated:\n\n- native delegate and class-local event declarations;\n- native lambda syntax trees with original source spans;\n- exact method-group and lambda signature validation;\n- deterministic source-order subscription slots represented by compiler-owned fields;\n- field/`this`-capturing lambdas lowered to synthetic instance methods;\n- event invocation lowered through Typed MIR branches and ordinary calls;\n- synthetic slots and methods excluded from LSP/DAP user surfaces.\n\nFirst-class delegate values, arbitrary local captures, heap closures, and general event storage remain Phase 20 work.\n")
replace_once(
    "docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md",
    "- LSP/DAP/hot reload operate on original source constructs and stable compiler-owned synthetic symbols.\n",
    "- language metadata is module-scoped in MIR, persisted in `.rsbc` 0.6, and emitted in the AOT manifest;\n- LSP/DAP operate on original source constructs without exposing compiler-owned names;\n- hot reload rejects semantic language-metadata changes while permitting body-only edits.\n")

# Remove all temporary migration carriers from the final product branch.
for temporary in [
    ROOT / "tools/ci/apply_phase18_metadata_artifacts.py",
    Path(__file__),
]:
    if temporary.exists():
        temporary.unlink()
