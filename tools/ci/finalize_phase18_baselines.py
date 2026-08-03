#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, content: str) -> None:
    (ROOT / path).write_text(content, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    content = read(path)
    if new in content:
        return
    if old not in content:
        raise RuntimeError(f"anchor not found in {path}: {old[:160]!r}")
    write(path, content.replace(old, new, 1))


# Update historical bytecode assertions now that metadata requires format 0.6.
for path in sorted((ROOT / "tests").glob("*.cpp")):
    content = path.read_text(encoding="utf-8")
    updated = content.replace("version.minor == 5", "version.minor == 6")
    updated = updated.replace(".rsbc 0.5", ".rsbc 0.6")
    updated = updated.replace("format 0.5", "format 0.6")
    if updated != content:
        path.write_text(updated, encoding="utf-8")

# Only original workspace symbols belong in LanguageService/LSP views.
replace_once(
    "src/tooling/LanguageService.cpp",
    '''std::vector<DocumentSymbol> LanguageService::allDefinitions() const {
    std::vector<DocumentSymbol> symbols;
    std::unordered_set<semantic::SymbolId> seen;
    auto append = [&](semantic::SymbolId id, semantic::SymbolKind kind,
                      std::string name, std::string detail,
                      const std::string& path, text::TextSpan span) {
        if (id == 0 || path.empty() || !seen.insert(id).second) return;
        symbols.push_back({id, kind, std::move(name), std::move(detail), {path, rangeFor(path, span)}});
    };
''',
    '''std::vector<DocumentSymbol> LanguageService::allDefinitions() const {
    std::vector<DocumentSymbol> symbols;
    std::unordered_set<semantic::SymbolId> seen;
    std::unordered_set<std::string> generatedNames;
    for (const auto& module : result_.modules) {
        for (const auto& instance :
             module.languageMetadata.genericInstantiations) {
            generatedNames.insert(instance.generatedName);
        }
    }
    auto append = [&](semantic::SymbolId id, semantic::SymbolKind kind,
                      std::string name, std::string detail,
                      const std::string& path, text::TextSpan span) {
        if (id == 0 || path.empty() ||
            documents_.find(path) == documents_.end() ||
            !identifier(name) || generatedNames.find(name) != generatedNames.end() ||
            !seen.insert(id).second) {
            return;
        }
        symbols.push_back({id, kind, std::move(name), std::move(detail), {path, rangeFor(path, span)}});
    };
''')
replace_once(
    "src/tooling/LanguageService.cpp",
    '''        for (const auto& type : module.types) {
            append(type.id, semantic::SymbolKind::Type, type.name,
                semantic::canonicalTypeName(type), type.sourceName, type.declarationSpan);
            for (const auto& field : type.fields) {
                append(field.id, semantic::SymbolKind::Field, field.name,
                    typeDetail(field.type, field.typeName), field.sourceName, field.declarationSpan);
''',
    '''        for (const auto& type : module.types) {
            if (type.synthetic) continue;
            append(type.id, semantic::SymbolKind::Type, type.name,
                semantic::canonicalTypeName(type), type.sourceName, type.declarationSpan);
            for (const auto& field : type.fields) {
                if (field.synthetic) continue;
                append(field.id, semantic::SymbolKind::Field, field.name,
                    typeDetail(field.type, field.typeName), field.sourceName, field.declarationSpan);
''')
replace_once(
    "src/tooling/LanguageService.cpp",
    '''            for (const auto& method : type.methods) {
                append(method.id, semantic::SymbolKind::Function, method.name,
                    functionDetail(method), method.sourceName, method.declarationSpan);
            }
            for (const auto& constructor : type.constructors) {
                append(constructor.id, semantic::SymbolKind::Function, type.name,
                    functionDetail(constructor), constructor.sourceName, constructor.declarationSpan);
            }
''',
    '''            for (const auto& method : type.methods) {
                if (method.synthetic) continue;
                append(method.id, semantic::SymbolKind::Function, method.name,
                    functionDetail(method), method.sourceName, method.declarationSpan);
            }
            for (const auto& constructor : type.constructors) {
                if (constructor.synthetic) continue;
                append(constructor.id, semantic::SymbolKind::Function, type.name,
                    functionDetail(constructor), constructor.sourceName, constructor.declarationSpan);
            }
''')

# Guard against leaving stale 0.5 assertions in C++ tests.
remaining = []
for path in sorted((ROOT / "tests").glob("*.cpp")):
    content = path.read_text(encoding="utf-8")
    if "version.minor == 5" in content or ".rsbc 0.5" in content or "format 0.5" in content:
        remaining.append(str(path.relative_to(ROOT)))
if remaining:
    raise RuntimeError("stale bytecode 0.5 assertions remain: " + ", ".join(remaining))

# The migration carrier deletes itself; snapshots are generated by the workflow.
Path(__file__).unlink()
