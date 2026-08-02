#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
path = root / "src/semantic/SemanticBinding.cpp"
text = path.read_text(encoding="utf-8")

old = '''    result->collectionVariable.index = nextVariableIndex_++;
    result->collectionVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->collectionVariable.index) + ":" +
        result->collectionVariable.name);
    (void)declareVariable(result->collectionVariable, {});

    result->indexVariable.name = "$foreach_index_" + std::to_string(nextVariableIndex_);
    result->indexVariable.type = PrimitiveType::Int;
    result->indexVariable.index = nextVariableIndex_++;
    result->indexVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->indexVariable.index) + ":" +
        result->indexVariable.name);
    (void)declareVariable(result->indexVariable, {});
'''
new = '''    result->collectionVariable.index = nextVariableIndex_++;
    result->collectionVariable.declarationSpan =
        syntaxTree.foreachKeyword.span;
    result->collectionVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->collectionVariable.index) + ":" +
        result->collectionVariable.name);
    (void)declareVariable(
        result->collectionVariable,
        syntaxTree.foreachKeyword.span);

    result->indexVariable.name = "$foreach_index_" + std::to_string(nextVariableIndex_);
    result->indexVariable.type = PrimitiveType::Int;
    result->indexVariable.index = nextVariableIndex_++;
    result->indexVariable.declarationSpan = syntaxTree.inKeyword.span;
    result->indexVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->indexVariable.index) + ":" +
        result->indexVariable.name);
    (void)declareVariable(
        result->indexVariable,
        syntaxTree.inKeyword.span);
'''
if old not in text:
    raise RuntimeError("foreach hidden-local anchor not found")
path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
print("Phase 18A foreach debug metadata fixed")
