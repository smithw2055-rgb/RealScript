#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]

semantic_path = root / "src/semantic/SemanticExpressions.cpp"
semantic = semantic_path.read_text(encoding="utf-8")
old = '''        if (index >= argumentModifiers.size() ||
            syntaxModifier(argumentModifiers[index]) !=
                parameter.modifier ||
            syntaxArguments[index]->kind() !=
                syntax::SyntaxKind::NameExpression) {
            diagnostics_.report(
                "RS8703",
                "reference argument must use the matching modifier "
                "and name a variable",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        const auto& name = static_cast<
'''
new = '''        if (index >= argumentModifiers.size() ||
            syntaxModifier(argumentModifiers[index]) !=
                parameter.modifier) {
            diagnostics_.report(
                "RS8703",
                "reference argument must use the matching modifier",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        if (parameter.modifier == ParameterModifier::In) {
            argument.value = convertExpression(
                std::move(arguments[index]),
                parameter.type,
                syntaxArguments[index]->span(),
                context,
                parameter.typeName);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        if (syntaxArguments[index]->kind() !=
            syntax::SyntaxKind::NameExpression) {
            diagnostics_.report(
                "RS8703",
                "ref and out arguments must name a variable",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        const auto& name = static_cast<
'''
if new not in semantic:
    if old not in semantic:
        raise RuntimeError("native reference call anchor not found")
    semantic = semantic.replace(old, new, 1)
# Remove now-unreachable later in branch for in.
old_late = '''        argument.variable = *variable;
        if (parameter.modifier == ParameterModifier::In) {
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        const auto wrapper = visibleTypes_.find(
'''
new_late = '''        argument.variable = *variable;

        const auto wrapper = visibleTypes_.find(
'''
if new_late not in semantic:
    if old_late not in semantic:
        raise RuntimeError("late in argument branch anchor not found")
    semantic = semantic.replace(old_late, new_late, 1)
semantic_path.write_text(semantic, encoding="utf-8")

mir_path = root / "src/mir/MirLowerer.cpp"
mir = mir_path.read_text(encoding="utf-8")
old_debug = '''        local.name = variable.name; local.slot = static_cast<std::uint32_t>(variable.index);
        local.type = variable.type;
        local.typeId = semantic::isExactType(variable.type) ? semantic::stableTypeId(variable.typeName) : 0;
'''
new_debug = '''        local.name = variable.name; local.slot = static_cast<std::uint32_t>(variable.index);
        const auto debugType = variable.parameter
            ? semantic::storageTypeOf(variable)
            : variable.type;
        const auto& debugTypeName = variable.parameter
            ? semantic::storageTypeNameOf(variable)
            : variable.typeName;
        local.type = debugType;
        local.typeId = semantic::isExactType(debugType)
            ? semantic::stableTypeId(debugTypeName)
            : 0;
'''
if new_debug not in mir:
    if old_debug not in mir:
        raise RuntimeError("debug local type anchor not found")
    mir = mir.replace(old_debug, new_debug, 1)
mir_path.write_text(mir, encoding="utf-8")

print("native reference fixes applied")
