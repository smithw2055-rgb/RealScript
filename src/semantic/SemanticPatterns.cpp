#include "SemanticInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace realscript::semantic {

using namespace detail;

std::unique_ptr<BoundPattern> Binder::bindPattern(
    const syntax::PatternSyntax& syntaxTree,
    PrimitiveType inputType,
    const std::string& inputTypeName,
    bool allowVariables,
    bool isSwitchPattern) {
    auto pat = std::make_unique<BoundPattern>();
    pat->span = syntaxTree.span();
    pat->inputType = inputType;
    pat->inputTypeName = inputTypeName;

    if (inputType == PrimitiveType::Error) {
        pat->kind = BoundPatternKind::Discard;
        return pat;
    }

    // Check if inputType is a Nullable<T> struct wrapper
    bool inputIsNullableStruct = false;
    TypeSymbol nullableDesc;
    FieldSymbol nullableHasVal;
    FieldSymbol nullableVal;
    if (inputType == PrimitiveType::Struct && !inputTypeName.empty()) {
        const auto found = visibleTypes_.find(inputTypeName);
        if (found != visibleTypes_.end() &&
            found->second.name.rfind("Nullable__", 0) == 0 &&
            found->second.fields.size() >= 2) {
            inputIsNullableStruct = true;
            nullableDesc = found->second;
            nullableHasVal = found->second.fields[0];
            nullableVal = found->second.fields[1];
        }
    }

    const auto declarePatternDesignation = [&](
        const syntax::SyntaxToken& tok,
        PrimitiveType varType,
        const std::string& varTypeName) -> std::optional<VariableSymbol> {
        if (tok.text.empty() || tok.text == "_") return std::nullopt;
        if (!allowVariables) {
            diagnostics_.report(
                "RS8935",
                "pattern variable '" + tok.text +
                    "' cannot be introduced inside a negated or disjunctive ('or') pattern",
                tok.span);
            return std::nullopt;
        }
        VariableSymbol variable;
        variable.name = tok.text;
        variable.type = varType;
        variable.typeName = varTypeName;
        variable.storageType = varType;
        variable.storageTypeName = varTypeName;
        variable.index = nextVariableIndex_++;
        if (declareVariable(variable, tok.span)) {
            if (const auto* declared = lookupVariable(variable.name)) {
                return *declared;
            }
        }
        return std::nullopt;
    };

    const auto bindConstantComparison = [&](const syntax::ExpressionSyntax& constSyntax) {
        pat->kind = BoundPatternKind::Constant;
        pat->comparisonOperator = BoundBinaryOperatorKind::Equals;
        syntax::TokenValue evalVal;
        PrimitiveType evalType = PrimitiveType::Error;
        const TypeSymbol* ownerPtr = currentOwnerType_ ? &(*currentOwnerType_) : nullptr;
        const bool isConst = evaluateConstantSyntaxExpression(
            constSyntax, ownerPtr, visibleTypes_, evalVal, evalType, &scopes_);
        auto boundConst = bindExpression(constSyntax);
        if (boundConst->type == PrimitiveType::Error) return;
        if (!isConst && boundConst->type != PrimitiveType::Null) {
            diagnostics_.report(
                "RS8936",
                "pattern constant requires a compile-time constant value",
                constSyntax.span());
            return;
        }
        if (boundConst->type == PrimitiveType::Null) {
            if (inputIsNullableStruct) {
                pat->unwrapsNullableStruct = true;
                pat->nullableStructType = nullableDesc;
                pat->nullableHasValueField = nullableHasVal;
                pat->nullableValueField = nullableVal;
                pat->constantNullCheck = true;
                return;
            }
            if (!isReferenceType(inputType)) {
                diagnostics_.report(
                    "RS8921",
                    "cannot match 'null' pattern against non-nullable value type '" +
                        (inputTypeName.empty()
                            ? std::string(primitiveTypeName(inputType))
                            : inputTypeName) + "'",
                    constSyntax.span());
                return;
            }
            pat->constantNullCheck = true;
            pat->constantValue = convertExpression(
                std::move(boundConst),
                inputType,
                constSyntax.span(),
                "null pattern",
                inputTypeName);
            pat->comparisonType = inputType;
            return;
        }

        PrimitiveType effectiveInput = inputType;
        std::string effectiveInputName = inputTypeName;
        if (inputIsNullableStruct) {
            pat->unwrapsNullableStruct = true;
            pat->nullableStructType = nullableDesc;
            pat->nullableHasValueField = nullableHasVal;
            pat->nullableValueField = nullableVal;
            pat->unwrappedTempVariable = allocateTempVariable(
                "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
            effectiveInput = nullableVal.type;
            effectiveInputName = nullableVal.typeName;
        }

        // Convert constant to effectiveInput (or common comparison type)
        if (effectiveInput == PrimitiveType::Object && effectiveInputName.empty()) {
            diagnostics_.report(
                "RS8921",
                "constant pattern cannot be applied directly to untyped object",
                constSyntax.span());
            return;
        }
        if (effectiveInput == PrimitiveType::Enum) {
            pat->comparisonType = PrimitiveType::Enum;
            pat->constantValue = convertExpression(
                std::move(boundConst),
                effectiveInput,
                constSyntax.span(),
                "constant pattern",
                effectiveInputName);
            return;
        }
        if ((effectiveInput == PrimitiveType::Char || isNumericType(effectiveInput)) &&
            (boundConst->type == PrimitiveType::Char || isNumericType(boundConst->type))) {
            if (effectiveInput == PrimitiveType::Char) {
                pat->comparisonType = PrimitiveType::Int;
                pat->constantValue = convertExpression(
                    std::move(boundConst),
                    PrimitiveType::Int,
                    constSyntax.span(),
                    "constant pattern");
                return;
            }
            if ((effectiveInput == PrimitiveType::ULong || effectiveInput == PrimitiveType::UInt) &&
                std::holds_alternative<std::int64_t>(evalVal) &&
                std::get<std::int64_t>(evalVal) >= 0 &&
                (effectiveInput == PrimitiveType::ULong ||
                 static_cast<std::uint64_t>(std::get<std::int64_t>(evalVal)) <=
                     static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))) {
                auto lit = std::make_unique<BoundLiteralExpression>();
                lit->span = constSyntax.span();
                lit->type = effectiveInput;
                lit->value = evalVal;
                pat->comparisonType = effectiveInput;
                pat->constantValue = std::move(lit);
                return;
            }
            PrimitiveType carrier = commonNumericType(effectiveInput, boundConst->type);
            if (carrier == PrimitiveType::Byte || carrier == PrimitiveType::SByte ||
                carrier == PrimitiveType::Short || carrier == PrimitiveType::UShort ||
                carrier == PrimitiveType::Char) {
                carrier = PrimitiveType::Int;
            }
            if (carrier != PrimitiveType::Error &&
                (effectiveInput == PrimitiveType::Byte ||
                 effectiveInput == PrimitiveType::SByte ||
                 effectiveInput == PrimitiveType::Short ||
                 effectiveInput == PrimitiveType::UShort)) {
                pat->comparisonType = carrier;
                pat->constantValue = convertExpression(
                    std::move(boundConst),
                    carrier,
                    constSyntax.span(),
                    "constant pattern");
                return;
            }
            pat->comparisonType = effectiveInput;
            pat->constantValue = convertExpression(
                std::move(boundConst),
                effectiveInput,
                constSyntax.span(),
                "constant pattern",
                effectiveInputName);
            return;
        }
        pat->comparisonType = effectiveInput;
        pat->constantValue = convertExpression(
            std::move(boundConst),
            effectiveInput,
            constSyntax.span(),
            "constant pattern",
            effectiveInputName);
    };

    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::ParenthesizedPattern: {
        const auto& paren = static_cast<const syntax::ParenthesizedPatternSyntax&>(syntaxTree);
        if (!paren.pattern) return pat;
        return bindPattern(*paren.pattern, inputType, inputTypeName, allowVariables, isSwitchPattern);
    }
    case syntax::SyntaxKind::DiscardPattern: {
        pat->kind = BoundPatternKind::Discard;
        return pat;
    }
    case syntax::SyntaxKind::VarPattern: {
        const auto& vp = static_cast<const syntax::VarPatternSyntax&>(syntaxTree);
        pat->kind = BoundPatternKind::Var;
        pat->variable = declarePatternDesignation(
            vp.designationToken, inputType, inputTypeName);
        return pat;
    }
    case syntax::SyntaxKind::ConstantPattern: {
        const auto& cp = static_cast<const syntax::ConstantPatternSyntax&>(syntaxTree);
        if (cp.expression) bindConstantComparison(*cp.expression);
        return pat;
    }
    case syntax::SyntaxKind::RelationalPattern: {
        const auto& rp = static_cast<const syntax::RelationalPatternSyntax&>(syntaxTree);
        pat->kind = BoundPatternKind::Relational;
        switch (rp.operatorToken.kind) {
        case syntax::SyntaxKind::LessToken:
            pat->comparisonOperator = BoundBinaryOperatorKind::Less;
            break;
        case syntax::SyntaxKind::LessOrEqualsToken:
            pat->comparisonOperator = BoundBinaryOperatorKind::LessOrEquals;
            break;
        case syntax::SyntaxKind::GreaterToken:
            pat->comparisonOperator = BoundBinaryOperatorKind::Greater;
            break;
        default:
            pat->comparisonOperator = BoundBinaryOperatorKind::GreaterOrEquals;
            break;
        }
        if (!rp.operand) return pat;

        syntax::TokenValue evalVal;
        PrimitiveType evalType = PrimitiveType::Error;
        const TypeSymbol* ownerPtr = currentOwnerType_ ? &(*currentOwnerType_) : nullptr;
        const bool isConst = evaluateConstantSyntaxExpression(
            *rp.operand, ownerPtr, visibleTypes_, evalVal, evalType, &scopes_);
        auto boundConst = bindExpression(*rp.operand);
        if (boundConst->type == PrimitiveType::Error) return pat;
        if (!isConst || boundConst->type == PrimitiveType::Null) {
            diagnostics_.report(
                "RS8936",
                "relational pattern requires a compile-time constant operand",
                rp.operand->span());
            return pat;
        }

        PrimitiveType effectiveInput = inputType;
        std::string effectiveInputName = inputTypeName;
        if (inputIsNullableStruct) {
            pat->unwrapsNullableStruct = true;
            pat->nullableStructType = nullableDesc;
            pat->nullableHasValueField = nullableHasVal;
            pat->nullableValueField = nullableVal;
            pat->unwrappedTempVariable = allocateTempVariable(
                "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
            effectiveInput = nullableVal.type;
            effectiveInputName = nullableVal.typeName;
        }

        if (effectiveInput == PrimitiveType::Enum) {
            if (boundConst->type != PrimitiveType::Enum ||
                (!effectiveInputName.empty() && !boundConst->typeName.empty() &&
                 effectiveInputName != boundConst->typeName)) {
                diagnostics_.report(
                    "RS8937",
                    "enum relational pattern requires a constant of the same enum type '" +
                        effectiveInputName + "'",
                    rp.operand->span());
                return pat;
            }
            const std::int64_t rhsVal = std::holds_alternative<std::int64_t>(evalVal)
                ? std::get<std::int64_t>(evalVal)
                : std::int64_t{0};
            std::vector<std::int64_t> domainValues{0};
            const auto enumIt = visibleTypes_.find(effectiveInputName);
            if (enumIt != visibleTypes_.end()) {
                for (const auto& em : enumIt->second.enumMembers) {
                    if (std::find(domainValues.begin(), domainValues.end(), em.value) ==
                        domainValues.end()) {
                        domainValues.push_back(em.value);
                    }
                }
            }
            std::vector<std::int64_t> matchingVals;
            std::vector<std::int64_t> nonMatchingVals;
            for (const auto v : domainValues) {
                bool ok = false;
                switch (pat->comparisonOperator) {
                case BoundBinaryOperatorKind::Less: ok = (v < rhsVal); break;
                case BoundBinaryOperatorKind::LessOrEquals: ok = (v <= rhsVal); break;
                case BoundBinaryOperatorKind::Greater: ok = (v > rhsVal); break;
                case BoundBinaryOperatorKind::GreaterOrEquals: ok = (v >= rhsVal); break;
                default: break;
                }
                if (ok) matchingVals.push_back(v);
                else nonMatchingVals.push_back(v);
            }
            auto makeEnumEqPat = [&](std::int64_t v) {
                auto eqPat = std::make_unique<BoundPattern>();
                eqPat->kind = BoundPatternKind::Constant;
                eqPat->span = rp.span();
                eqPat->inputType = effectiveInput;
                eqPat->inputTypeName = effectiveInputName;
                eqPat->comparisonOperator = BoundBinaryOperatorKind::Equals;
                eqPat->comparisonType = PrimitiveType::Enum;
                auto enumLit = std::make_unique<BoundLiteralExpression>();
                enumLit->span = rp.operand->span();
                enumLit->type = PrimitiveType::Enum;
                enumLit->typeName = effectiveInputName;
                enumLit->value = v;
                eqPat->constantValue = std::move(enumLit);
                return eqPat;
            };
            auto buildOrTree = [&](const std::vector<std::int64_t>& vals) {
                auto acc = makeEnumEqPat(vals.front());
                for (std::size_t idx = 1; idx < vals.size(); ++idx) {
                    auto orNode = std::make_unique<BoundPattern>();
                    orNode->kind = BoundPatternKind::LogicalOr;
                    orNode->span = rp.span();
                    orNode->inputType = effectiveInput;
                    orNode->inputTypeName = effectiveInputName;
                    orNode->left = std::move(acc);
                    orNode->right = makeEnumEqPat(vals[idx]);
                    acc = std::move(orNode);
                }
                return acc;
            };
            if (matchingVals.empty()) {
                pat->kind = BoundPatternKind::Constant;
                pat->hasStaticBool = true;
                pat->staticBoolValue = false;
                return pat;
            }
            if (nonMatchingVals.empty()) {
                pat->kind = BoundPatternKind::Constant;
                pat->hasStaticBool = true;
                pat->staticBoolValue = true;
                return pat;
            }
            if (matchingVals.size() <= nonMatchingVals.size()) {
                auto built = buildOrTree(matchingVals);
                built->unwrapsNullableStruct = pat->unwrapsNullableStruct;
                built->nullableStructType = pat->nullableStructType;
                built->nullableHasValueField = pat->nullableHasValueField;
                built->nullableValueField = pat->nullableValueField;
                built->unwrappedTempVariable = pat->unwrappedTempVariable;
                built->inputType = inputType;
                built->inputTypeName = inputTypeName;
                return built;
            }
            auto notNode = std::make_unique<BoundPattern>();
            notNode->kind = BoundPatternKind::LogicalNot;
            notNode->span = rp.span();
            notNode->inputType = inputType;
            notNode->inputTypeName = inputTypeName;
            notNode->unwrapsNullableStruct = pat->unwrapsNullableStruct;
            notNode->nullableStructType = pat->nullableStructType;
            notNode->nullableHasValueField = pat->nullableHasValueField;
            notNode->nullableValueField = pat->nullableValueField;
            notNode->unwrappedTempVariable = pat->unwrappedTempVariable;
            notNode->left = buildOrTree(nonMatchingVals);
            return notNode;
        }

        if (!isNumericType(effectiveInput) || !isNumericType(boundConst->type)) {
            diagnostics_.report(
                "RS8937",
                "relational pattern is only valid on numeric, char, or enum values",
                rp.span());
            return pat;
        }

        if (effectiveInput == PrimitiveType::Char) {
            if (boundConst->type != PrimitiveType::Char && !isIntegralType(boundConst->type)) {
                diagnostics_.report(
                    "RS8937",
                    "char relational pattern requires a char or integral constant",
                    rp.operand->span());
                return pat;
            }
            pat->comparisonType = PrimitiveType::Int;
            pat->constantValue = convertExpression(
                std::move(boundConst),
                PrimitiveType::Int,
                rp.operand->span(),
                "relational pattern operand");
            return pat;
        }

        if (effectiveInput == PrimitiveType::ULong) {
            if (isFloatingPointType(boundConst->type)) {
                pat->comparisonType = PrimitiveType::Double;
                pat->constantValue = convertExpression(
                    std::move(boundConst),
                    PrimitiveType::Double,
                    rp.operand->span(),
                    "relational pattern operand");
                return pat;
            }
            if (boundConst->type != PrimitiveType::ULong &&
                std::holds_alternative<std::int64_t>(evalVal) &&
                std::get<std::int64_t>(evalVal) < 0) {
                diagnostics_.report(
                    "RS8937",
                    "cannot compare unsigned 'ulong' with negative constant in relational pattern",
                    rp.operand->span());
                return pat;
            }
            pat->comparisonType = PrimitiveType::ULong;
            auto ulLit = std::make_unique<BoundLiteralExpression>();
            ulLit->span = rp.operand->span();
            ulLit->type = PrimitiveType::ULong;
            ulLit->value = evalVal;
            pat->constantValue = std::move(ulLit);
            return pat;
        }

        if (effectiveInput == PrimitiveType::UInt) {
            if (boundConst->type == PrimitiveType::UInt ||
                (isIntegralType(boundConst->type) &&
                 std::holds_alternative<std::int64_t>(evalVal) &&
                 std::get<std::int64_t>(evalVal) >= 0 &&
                 static_cast<std::uint64_t>(std::get<std::int64_t>(evalVal)) <=
                     static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))) {
                pat->comparisonType = PrimitiveType::UInt;
                auto uiLit = std::make_unique<BoundLiteralExpression>();
                uiLit->span = rp.operand->span();
                uiLit->type = PrimitiveType::UInt;
                uiLit->value = evalVal;
                pat->constantValue = std::move(uiLit);
                return pat;
            }
        }

        // General signed/floating carrier: Int, Long, Float, or Double
        PrimitiveType carrier = commonNumericType(effectiveInput, boundConst->type);
        if (carrier == PrimitiveType::Error) {
            diagnostics_.report(
                "RS8937",
                "incompatible types in relational pattern",
                rp.span());
            return pat;
        }
        if (carrier == PrimitiveType::Byte || carrier == PrimitiveType::SByte ||
            carrier == PrimitiveType::Short || carrier == PrimitiveType::UShort ||
            carrier == PrimitiveType::Char) {
            carrier = PrimitiveType::Int;
        }
        pat->comparisonType = carrier;
        pat->constantValue = convertExpression(
            std::move(boundConst),
            carrier,
            rp.operand->span(),
            "relational pattern operand");
        return pat;
    }
    case syntax::SyntaxKind::UnaryPattern: {
        const auto& un = static_cast<const syntax::UnaryPatternSyntax&>(syntaxTree);
        pat->kind = BoundPatternKind::LogicalNot;
        if (un.pattern) {
            pat->left = bindPattern(*un.pattern, inputType, inputTypeName, false, isSwitchPattern);
        }
        return pat;
    }
    case syntax::SyntaxKind::BinaryPattern: {
        const auto& bin = static_cast<const syntax::BinaryPatternSyntax&>(syntaxTree);
        const bool isAnd = (bin.operatorToken.text == "and");
        pat->kind = isAnd ? BoundPatternKind::LogicalAnd : BoundPatternKind::LogicalOr;
        const bool childAllowVars = isAnd && allowVariables;
        if (bin.left) {
            pat->left = bindPattern(*bin.left, inputType, inputTypeName, childAllowVars, isSwitchPattern);
        }
        if (bin.right) {
            pat->right = bindPattern(*bin.right, inputType, inputTypeName, childAllowVars, isSwitchPattern);
        }
        return pat;
    }
    case syntax::SyntaxKind::TypePattern: {
        const auto& tp = static_cast<const syntax::TypePatternSyntax&>(syntaxTree);
        // Check if bare identifier without designation refers to a local const or const field
        if (!tp.designationToken && !tp.type.isGeneric() &&
            !tp.type.isArray() && !tp.type.isNullable() &&
            resolvePrimitiveType(tp.type.name.text) == PrimitiveType::Error) {
            const auto* localVar = lookupVariable(tp.type.name.text);
            const FieldSymbol* constFld = (!localVar && currentOwnerType_)
                ? findField(*currentOwnerType_, tp.type.name.text)
                : nullptr;
            if ((localVar && localVar->isConst) ||
                (constFld && constFld->constField)) {
                syntax::NameExpressionSyntax nameSyn;
                nameSyn.identifierToken = tp.type.name;
                bindConstantComparison(nameSyn);
                return pat;
            }
        }

        pat->kind = BoundPatternKind::Type;
        pat->hasTypeCheck = true;
        pat->targetType = bindType(tp.type, false, &pat->targetTypeName);
        if (pat->targetType == PrimitiveType::Error) return pat;
        pat->targetTypeId = isExactType(pat->targetType) && !pat->targetTypeName.empty()
            ? stableTypeId(pat->targetTypeName)
            : 0;

        PrimitiveType effectiveInput = inputType;
        std::string effectiveInputName = inputTypeName;
        if (inputIsNullableStruct &&
            (pat->targetType != PrimitiveType::Struct || pat->targetTypeName != inputTypeName)) {
            pat->unwrapsNullableStruct = true;
            pat->nullableStructType = nullableDesc;
            pat->nullableHasValueField = nullableHasVal;
            pat->nullableValueField = nullableVal;
            pat->unwrappedTempVariable = allocateTempVariable(
                "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
            effectiveInput = nullableVal.type;
            effectiveInputName = nullableVal.typeName;
        }

        if (isReferenceType(effectiveInput) && isReferenceType(pat->targetType)) {
            pat->staticTypeMatch = false;
        } else if (!isSwitchPattern && isReferenceType(effectiveInput)) {
            // In `is` expressions, reference/boxed operands may test against value types via Opcode::IsType
            pat->staticTypeMatch = false;
            if (tp.designationToken) {
                diagnostics_.report(
                    "RS8920",
                    "type pattern designation requires an 'is' reference-type pattern",
                    tp.span());
                return pat;
            }
        } else if (effectiveInput == pat->targetType &&
                   (!isExactType(effectiveInput) || effectiveInputName == pat->targetTypeName) &&
                   (!isSwitchPattern || inputIsNullableStruct || !tp.designationToken || isReferenceType(pat->targetType))) {
            pat->staticTypeMatch = true;
        } else if (!isSwitchPattern && !tp.designationToken) {
            // Value-type tested against unrelated type in `is` expression (e.g. generic specialization `val is RefType`)
            pat->kind = BoundPatternKind::Constant;
            pat->hasStaticBool = true;
            pat->staticBoolValue = false;
            return pat;
        } else {
            diagnostics_.report(
                "RS8921",
                "switch type patterns require reference types",
                tp.span());
            return pat;
        }

        if (tp.designationToken) {
            if (!isSwitchPattern && !isReferenceType(pat->targetType) && !inputIsNullableStruct && !pat->staticTypeMatch) {
                diagnostics_.report(
                    "RS8920",
                    "type pattern designation requires an 'is' reference-type pattern",
                    tp.span());
                return pat;
            }
            pat->variable = declarePatternDesignation(
                *tp.designationToken, pat->targetType, pat->targetTypeName);
        }
        return pat;
    }
    case syntax::SyntaxKind::PropertyPattern: {
        const auto& pp = static_cast<const syntax::PropertyPatternSyntax&>(syntaxTree);
        pat->kind = BoundPatternKind::Property;

        PrimitiveType effectiveType = inputType;
        std::string effectiveTypeName = inputTypeName;

        if (pp.type) {
            pat->hasTypeCheck = true;
            pat->targetType = bindType(*pp.type, false, &pat->targetTypeName);
            if (pat->targetType == PrimitiveType::Error) return pat;
            pat->targetTypeId = isExactType(pat->targetType) && !pat->targetTypeName.empty()
                ? stableTypeId(pat->targetTypeName)
                : 0;

            PrimitiveType checkInput = inputType;
            std::string checkInputName = inputTypeName;
            if (inputIsNullableStruct &&
                (pat->targetType != PrimitiveType::Struct || pat->targetTypeName != inputTypeName)) {
                pat->unwrapsNullableStruct = true;
                pat->nullableStructType = nullableDesc;
                pat->nullableHasValueField = nullableHasVal;
                pat->nullableValueField = nullableVal;
                pat->unwrappedTempVariable = allocateTempVariable(
                    "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
                checkInput = nullableVal.type;
                checkInputName = nullableVal.typeName;
            }

            if (isReferenceType(checkInput) && isReferenceType(pat->targetType)) {
                pat->staticTypeMatch = false;
            } else if (checkInput == pat->targetType &&
                       (!isExactType(checkInput) || checkInputName == pat->targetTypeName)) {
                pat->staticTypeMatch = true;
            } else {
                diagnostics_.report(
                    "RS8921",
                    "cannot match property pattern type '" +
                        (pat->targetTypeName.empty()
                            ? std::string(primitiveTypeName(pat->targetType))
                            : pat->targetTypeName) +
                        "' against value of type '" +
                        (inputTypeName.empty()
                            ? std::string(primitiveTypeName(inputType))
                            : inputTypeName) + "'",
                    pp.type->span());
                return pat;
            }
            effectiveType = pat->targetType;
            effectiveTypeName = pat->targetTypeName;
        } else if (inputIsNullableStruct) {
            pat->unwrapsNullableStruct = true;
            pat->nullableStructType = nullableDesc;
            pat->nullableHasValueField = nullableHasVal;
            pat->nullableValueField = nullableVal;
            pat->unwrappedTempVariable = allocateTempVariable(
                "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
            effectiveType = nullableVal.type;
            effectiveTypeName = nullableVal.typeName;
            pat->targetType = effectiveType;
            pat->targetTypeName = effectiveTypeName;
            pat->targetTypeId = isExactType(effectiveType) && !effectiveTypeName.empty()
                ? stableTypeId(effectiveTypeName)
                : 0;
        } else {
            pat->targetType = effectiveType;
            pat->targetTypeName = effectiveTypeName;
            pat->targetTypeId = isExactType(effectiveType) && !effectiveTypeName.empty()
                ? stableTypeId(effectiveTypeName)
                : 0;
        }

        // Allocate temporary slot for narrowed/non-null receiver so subpattern accesses & designation reuse it
        pat->receiverTempVariable = allocateTempVariable(
            "pat_recv", effectiveType, effectiveTypeName, pp.span());

        for (const auto& subSyn : pp.properties) {
            BoundPropertySubpattern boundSub;
            boundSub.span = subSyn.span();

            auto makeRecvExpr = [&]() {
                return makeVariableAccess(*pat->receiverTempVariable, subSyn.memberToken.span);
            };

            const auto& memberName = subSyn.memberToken.text;
            std::unique_ptr<BoundExpression> accessExpr;
            if (effectiveType == PrimitiveType::Array &&
                (memberName == "Length" || memberName == "Count" || memberName == "length")) {
                auto arrLen = std::make_unique<BoundArrayLengthExpression>();
                arrLen->span = subSyn.memberToken.span;
                arrLen->type = PrimitiveType::Int;
                arrLen->receiver = makeRecvExpr();
                accessExpr = std::move(arrLen);
            } else if (effectiveType == PrimitiveType::String &&
                       (memberName == "Length" || memberName == "Count" || memberName == "length")) {
                auto call = std::make_unique<BoundCallExpression>();
                call->span = subSyn.memberToken.span;
                call->type = PrimitiveType::Int;
                call->function = makeIntrinsicFunction(
                    "$intrinsic::string_length",
                    PrimitiveType::Int,
                    "",
                    {{PrimitiveType::String, ""}});
                call->function.method = true;
                call->function.staticMethod = false;
                call->arguments.push_back(makeRecvExpr());
                accessExpr = std::move(call);
            } else if ((effectiveType == PrimitiveType::Object ||
                        effectiveType == PrimitiveType::Struct) &&
                       !effectiveTypeName.empty()) {
                const auto ownerIt = visibleTypes_.find(effectiveTypeName);
                if (ownerIt == visibleTypes_.end()) {
                    diagnostics_.report(
                        "RS2405",
                        "type descriptor '" + effectiveTypeName + "' is unavailable",
                        subSyn.memberToken.span);
                } else {
                    const auto& ownerDesc = ownerIt->second;
                    if (const auto* field = findField(ownerDesc, memberName)) {
                        if (!isMemberAccessible(field->accessibility, field->declaringTypeId)) {
                            diagnostics_.report(
                                "RS2534",
                                "field '" + field->name + "' is inaccessible",
                                subSyn.memberToken.span);
                        }
                        if (field->staticField || field->constField) {
                            diagnostics_.report(
                                "RS2484",
                                "property pattern cannot match static or const field '" +
                                    field->name + "'",
                                subSyn.memberToken.span);
                        }
                        checkObsoleteReference(
                            field->id, "field", field->name, subSyn.memberToken.span);
                        recordSymbolReference(
                            occurrences_,
                            currentSourceName_,
                            field->id,
                            SymbolKind::Field,
                            field->name,
                            subSyn.memberToken.span);
                        if (ownerDesc.kind == TypeKind::Struct) {
                            auto sfa = std::make_unique<BoundStructFieldAccessExpression>();
                            sfa->span = subSyn.memberToken.span;
                            sfa->type = field->type;
                            sfa->typeName = field->typeName;
                            sfa->receiver = makeRecvExpr();
                            sfa->ownerType = ownerDesc;
                            sfa->field = *field;
                            accessExpr = std::move(sfa);
                        } else {
                            auto mfa = std::make_unique<BoundMemberAccessExpression>();
                            mfa->span = subSyn.memberToken.span;
                            mfa->type = field->type;
                            mfa->typeName = field->typeName;
                            mfa->receiver = makeRecvExpr();
                            mfa->ownerType = ownerDesc;
                            mfa->field = *field;
                            accessExpr = std::move(mfa);
                        }
                    } else if (const auto* prop = findProperty(ownerDesc, memberName)) {
                        if (!isMemberAccessible(prop->accessibility, prop->declaringTypeId)) {
                            diagnostics_.report(
                                "RS2534",
                                "property '" + prop->name + "' is inaccessible",
                                subSyn.memberToken.span);
                        }
                        if (prop->staticProperty) {
                            diagnostics_.report(
                                "RS2484",
                                "property pattern requires an instance property, not static '" +
                                    prop->name + "'",
                                subSyn.memberToken.span);
                        } else if (!prop->getter) {
                            diagnostics_.report(
                                "RS2481",
                                "property '" + prop->name + "' is write-only",
                                subSyn.memberToken.span);
                        } else {
                            checkObsoleteReference(
                                prop->id, "property", prop->name, subSyn.memberToken.span);
                            recordSymbolReference(
                                occurrences_,
                                currentSourceName_,
                                prop->id,
                                SymbolKind::Property,
                                prop->name,
                                subSyn.memberToken.span);
                            std::vector<const syntax::ExpressionSyntax*> emptySyntaxArgs;
                            std::vector<std::optional<syntax::SyntaxToken>> emptyMods;
                            accessExpr = bindSelectedCall(
                                *prop->getter,
                                {},
                                emptySyntaxArgs,
                                emptyMods,
                                {},
                                makeRecvExpr(),
                                subSyn.memberToken.span,
                                "property pattern getter");
                        }
                    } else if (memberName == "Count" || memberName == "Length") {
                        auto getterMethods = findMethods(ownerDesc, memberName, false);
                        if (getterMethods.empty() && memberName == "Length") {
                            getterMethods = findMethods(ownerDesc, "Count", false);
                        }
                        for (const auto* m : getterMethods) {
                            if (m->parameters.size() == visibleParameterOffset(*m) &&
                                isMemberAccessible(m->accessibility, m->declaringTypeId, m->moduleName)) {
                                std::vector<const syntax::ExpressionSyntax*> emptySyntaxArgs;
                                std::vector<std::optional<syntax::SyntaxToken>> emptyMods;
                                accessExpr = bindSelectedCall(
                                    *m,
                                    {},
                                    emptySyntaxArgs,
                                    emptyMods,
                                    {},
                                    makeRecvExpr(),
                                    subSyn.memberToken.span,
                                    "property pattern getter");
                                break;
                            }
                        }
                    }
                    if (!accessExpr) {
                        diagnostics_.report(
                            "RS2406",
                            "type '" + effectiveTypeName + "' has no readable field or property '" +
                                memberName + "'",
                            subSyn.memberToken.span);
                    }
                }
            } else {
                diagnostics_.report(
                    "RS2404",
                    "property pattern member '" + memberName +
                        "' requires a class, struct, string, or array value",
                    subSyn.memberToken.span);
            }

            if (!accessExpr) {
                accessExpr = makeError(subSyn.memberToken.span);
            }
            const auto memberType = accessExpr->type;
            const auto memberTypeName = accessExpr->typeName;
            if (memberType != PrimitiveType::Error) {
                boundSub.memberTempVariable = allocateTempVariable(
                    "pat_mem", memberType, memberTypeName, subSyn.span());
            }
            boundSub.memberAccess = std::move(accessExpr);
            if (subSyn.pattern) {
                boundSub.pattern = bindPattern(
                    *subSyn.pattern, memberType, memberTypeName, allowVariables);
            }
            pat->properties.push_back(std::move(boundSub));
        }

        if (pp.designationToken) {
            pat->variable = declarePatternDesignation(
                *pp.designationToken, effectiveType, effectiveTypeName);
        }
        return pat;
    }
    default:
        return pat;
    }
}

std::unique_ptr<BoundExpression> Binder::bindTypeBinaryExpression(
    const syntax::TypeBinaryExpressionSyntax& syntaxTree) {
    auto operand = bindExpression(*syntaxTree.expression);
    if (operand->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }
    const bool safeCast = syntaxTree.operatorToken.kind ==
        syntax::SyntaxKind::AsKeyword;
    if (safeCast) {
        std::string targetTypeName;
        const auto targetType = bindType(
            syntaxTree.type, false, &targetTypeName);
        if (targetType == PrimitiveType::Error) {
            return makeError(syntaxTree.span());
        }
        if (!isReferenceType(operand->type) ||
            !isReferenceType(targetType)) {
            diagnostics_.report(
                "RS8910", "the 'as' operator requires reference types",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        auto result = std::make_unique<BoundTypeBinaryExpression>();
        result->span = syntaxTree.span();
        result->expression = std::move(operand);
        result->targetType = targetType;
        result->targetTypeName = targetTypeName;
        result->targetTypeId = isExactType(targetType)
            ? stableTypeId(targetTypeName)
            : 0;
        result->safeCast = true;
        result->type = targetType;
        result->typeName = targetTypeName;
        return result;
    }

    if (operand->type == PrimitiveType::Null) {
        operand = convertExpression(
            std::move(operand),
            PrimitiveType::Object,
            syntaxTree.expression->span(),
            "pattern operand");
    }

    auto result = std::make_unique<BoundTypeBinaryExpression>();
    result->span = syntaxTree.span();
    result->safeCast = false;
    result->type = PrimitiveType::Bool;
    result->patternInputVariable = allocateTempVariable(
        "is_input", operand->type, operand->typeName, syntaxTree.expression->span());
    const auto opType = operand->type;
    const auto opTypeName = operand->typeName;
    result->expression = std::move(operand);

    if (syntaxTree.pattern) {
        result->pattern = bindPattern(
            *syntaxTree.pattern, opType, opTypeName, true);
        if (result->pattern && result->pattern->variable) {
            result->patternVariable = result->pattern->variable;
        }
    } else {
        std::string targetTypeName;
        const auto targetType = bindType(syntaxTree.type, false, &targetTypeName);
        if (targetType == PrimitiveType::Error) return makeError(syntaxTree.span());
        result->targetType = targetType;
        result->targetTypeName = targetTypeName;
        result->targetTypeId = isExactType(targetType) ? stableTypeId(targetTypeName) : 0;
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindTaggedStringExpression(
    const syntax::TaggedStringExpressionSyntax& syntaxTree) {
    const std::string handlerName = "__TagLiteral_" + syntaxTree.tagToken.text;
    syntax::CallExpressionSyntax syntheticCall;
    syntheticCall.identifierToken = syntaxTree.tagToken;
    syntheticCall.identifierToken.text = handlerName;
    syntheticCall.openParenToken = syntax::SyntaxToken{
        syntax::SyntaxKind::OpenParenToken, syntaxTree.stringToken.span, "(", {}, true};
    auto argLit = std::make_unique<syntax::LiteralExpressionSyntax>();
    argLit->literalToken = syntaxTree.stringToken;
    syntheticCall.arguments.push_back(std::move(argLit));
    syntheticCall.argumentNames.push_back(std::nullopt);
    syntheticCall.argumentModifiers.push_back(std::nullopt);
    syntheticCall.closeParenToken = syntax::SyntaxToken{
        syntax::SyntaxKind::CloseParenToken, syntaxTree.stringToken.span, ")", {}, true};

    const bool hasGlobal = visibleFunctions_.find(handlerName) != visibleFunctions_.end();
    const bool hasMember = currentOwnerType_ &&
        (!findMethods(*currentOwnerType_, handlerName, true).empty() ||
         (!currentStaticMethod_ && !findMethods(*currentOwnerType_, handlerName, false).empty()));
    if (!hasGlobal && !hasMember) {
        diagnostics_.report(
            "RS2690",
            "unhandled tagged string literal '" + syntaxTree.tagToken.text +
                "@\"...\"'; import a module providing '" + handlerName +
                "(string)' or mount a compiler macro plugin",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    return bindCallExpression(syntheticCall);
}

std::unique_ptr<BoundExpression> Binder::bindTypeOfExpression(
    const syntax::TypeOfExpressionSyntax& syntaxTree) {
    std::string typeName;
    const auto type = bindType(syntaxTree.type, false, &typeName);
    if (type == PrimitiveType::Error) return makeError(syntaxTree.span());
    auto result = std::make_unique<BoundTypeOfExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::ULong;
    result->queriedType = type;
    result->queriedTypeName = typeName;
    result->queriedTypeId = isExactType(type)
        ? stableTypeId(typeName)
        : stableTypeId(
            "$primitive::" + std::string(primitiveTypeName(type)));
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindSwitchExpression(
    const syntax::SwitchExpressionSyntax& syntaxTree) {
    auto result = std::make_unique<BoundSwitchExpression>();
    result->span = syntaxTree.span();
    result->expression = bindExpression(*syntaxTree.expression);
    if (result->expression->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }
    if (result->expression->type == PrimitiveType::Null) {
        result->expression = convertExpression(
            std::move(result->expression),
            PrimitiveType::Object,
            syntaxTree.expression->span(),
            "switch expression");
    }
    result->valueVariable = allocateTempVariable(
        "sw_expr_val",
        result->expression->type,
        result->expression->typeName,
        syntaxTree.expression->span());

    PrimitiveType commonType = PrimitiveType::Error;
    std::string commonTypeName;
    bool hasCatchAll = false;
    bool hasExplicitDiscard = false;
    bool sawBoolTrue = false;
    bool sawBoolFalse = false;
    bool sawNullArm = false;
    bool sawNotNullArm = false;
    std::unordered_set<std::int64_t> coveredEnumValues;
    bool hasLowerUnbounded = false;
    std::int64_t maxLowerCovered = 0; // covers (-inf, maxLowerCovered] or track complement
    std::vector<std::pair<BoundBinaryOperatorKind, std::int64_t>> intRelArms;
    std::vector<std::pair<BoundBinaryOperatorKind, double>> floatRelArms;

    const auto isUnconditionalPattern = [](const BoundPattern& pat) noexcept -> bool {
        if (pat.kind == BoundPatternKind::Discard || pat.kind == BoundPatternKind::Var) {
            return true;
        }
        if (pat.kind == BoundPatternKind::Type &&
            pat.staticTypeMatch && !pat.unwrapsNullableStruct) {
            return true;
        }
        if (pat.kind == BoundPatternKind::Property &&
            pat.properties.empty() && !isReferenceType(pat.inputType) &&
            !pat.unwrapsNullableStruct && (!pat.hasTypeCheck || pat.staticTypeMatch)) {
            return true;
        }
        return false;
    };

    for (const auto& sourceArm : syntaxTree.arms) {
        BoundSwitchExpressionArm arm;
        arm.span = sourceArm.span();
        const bool explicitDiscard =
            sourceArm.discardToken.has_value() ||
            (sourceArm.pattern && sourceArm.pattern->kind() == syntax::SyntaxKind::DiscardPattern);
        arm.discard = explicitDiscard;
        if (explicitDiscard) {
            if (hasExplicitDiscard) {
                diagnostics_.report(
                    "RS8923", "switch expression has more than one discard arm",
                    sourceArm.span());
            }
            hasExplicitDiscard = true;
        }
        pushScope(sourceArm.span());
        if (sourceArm.pattern) {
            arm.pattern = bindPattern(
                *sourceArm.pattern,
                result->expression->type,
                result->expression->typeName,
                true,
                true);
        } else if (sourceArm.label) {
            arm.label = convertExpression(
                bindExpression(*sourceArm.label), result->expression->type,
                sourceArm.label->span(), "switch expression pattern",
                result->expression->typeName);
        }
        if (sourceArm.guard) {
            if (explicitDiscard) {
                diagnostics_.report(
                    "RS8925", "discard switch arm cannot have a guard",
                    sourceArm.guard->span());
            }
            arm.guard = convertExpression(
                bindExpression(*sourceArm.guard), PrimitiveType::Bool,
                sourceArm.guard->span(), "switch expression guard");
        } else {
            (void)hasLowerUnbounded;
            (void)maxLowerCovered;
            if (explicitDiscard) {
                hasCatchAll = true;
                arm.discard = true;
            } else if (arm.pattern && isUnconditionalPattern(*arm.pattern)) {
                hasCatchAll = true;
            } else if (arm.pattern) {
                const auto& p = *arm.pattern;
                if (p.kind == BoundPatternKind::Constant && p.constantNullCheck) {
                    sawNullArm = true;
                } else if (p.kind == BoundPatternKind::LogicalNot &&
                           p.left && p.left->kind == BoundPatternKind::Constant &&
                           p.left->constantNullCheck) {
                    sawNotNullArm = true;
                } else if (p.kind == BoundPatternKind::Property &&
                           p.properties.empty() &&
                           (!p.hasTypeCheck || p.staticTypeMatch)) {
                    sawNotNullArm = true;
                } else if (result->expression->type == PrimitiveType::Bool &&
                           p.kind == BoundPatternKind::Constant &&
                           p.constantValue &&
                           p.constantValue->kind() == BoundNodeKind::LiteralExpression) {
                    const auto& lit = static_cast<const BoundLiteralExpression&>(*p.constantValue);
                    if (const auto* bv = std::get_if<bool>(&lit.value)) {
                        if (*bv) sawBoolTrue = true;
                        else sawBoolFalse = true;
                    }
                } else if (result->expression->type == PrimitiveType::Enum &&
                           p.kind == BoundPatternKind::Constant &&
                           p.constantValue &&
                           p.constantValue->kind() == BoundNodeKind::LiteralExpression) {
                    const auto& lit = static_cast<const BoundLiteralExpression&>(*p.constantValue);
                    if (const auto* iv = std::get_if<std::int64_t>(&lit.value)) {
                        coveredEnumValues.insert(*iv);
                    }
                } else if (p.kind == BoundPatternKind::Relational &&
                           p.constantValue &&
                           p.constantValue->kind() == BoundNodeKind::LiteralExpression) {
                    const auto& lit = static_cast<const BoundLiteralExpression&>(*p.constantValue);
                    if (const auto* iv = std::get_if<std::int64_t>(&lit.value)) {
                        intRelArms.emplace_back(p.comparisonOperator, *iv);
                    } else if (const auto* dv = std::get_if<double>(&lit.value)) {
                        floatRelArms.emplace_back(p.comparisonOperator, *dv);
                    }
                }
            }
        }
        arm.value = bindExpression(*sourceArm.value);
        if (commonType == PrimitiveType::Error) {
            commonType = arm.value->type;
            commonTypeName = arm.value->typeName;
        } else if (commonType == arm.value->type &&
                   (!isExactType(commonType) ||
                    commonTypeName == arm.value->typeName)) {
            // exact match
        } else if (isNumericType(commonType) &&
                   isNumericType(arm.value->type)) {
            commonType = commonNumericType(commonType, arm.value->type);
            commonTypeName.clear();
        } else if (arm.value->type == PrimitiveType::Null &&
                   isReferenceType(commonType)) {
            // keep the current reference type
        } else if (commonType == PrimitiveType::Null &&
                   isReferenceType(arm.value->type)) {
            commonType = arm.value->type;
            commonTypeName = arm.value->typeName;
        } else if (commonType == PrimitiveType::Object &&
                   arm.value->type == PrimitiveType::Object &&
                   isAssignable(visibleTypes_, arm.value->typeName,
                                commonTypeName)) {
            // keep current base type
        } else if (commonType == PrimitiveType::Object &&
                   arm.value->type == PrimitiveType::Object &&
                   isAssignable(visibleTypes_, commonTypeName,
                                arm.value->typeName)) {
            commonTypeName = arm.value->typeName;
        } else {
            diagnostics_.report(
                "RS8924", "switch expression arms have no common type",
                sourceArm.value->span());
            commonType = PrimitiveType::Error;
        }
        popScope();
        result->arms.push_back(std::move(arm));
    }
    bool exhaustiveByCases =
        (result->expression->type == PrimitiveType::Bool && sawBoolTrue && sawBoolFalse) ||
        (sawNullArm && sawNotNullArm);
    if (!exhaustiveByCases && result->expression->type == PrimitiveType::Enum) {
        const auto enumIt = visibleTypes_.find(result->expression->typeName);
        if (enumIt != visibleTypes_.end() && !enumIt->second.enumMembers.empty()) {
            bool allEnumCovered = true;
            for (const auto& em : enumIt->second.enumMembers) {
                if (coveredEnumValues.find(em.value) == coveredEnumValues.end()) {
                    allEnumCovered = false;
                    break;
                }
            }
            if (allEnumCovered) exhaustiveByCases = true;
        }
    }
    if (!exhaustiveByCases && isIntegralType(result->expression->type) && !intRelArms.empty()) {
        // Check if (-inf, loMax] and [hiMin, +inf) meet or overlap: loMax + 1 >= hiMin
        bool hasLo = false;
        std::int64_t loMax = std::numeric_limits<std::int64_t>::min();
        bool hasHi = false;
        std::int64_t hiMin = std::numeric_limits<std::int64_t>::max();
        for (const auto& [op, val] : intRelArms) {
            if (op == BoundBinaryOperatorKind::Less && val > std::numeric_limits<std::int64_t>::min()) {
                loMax = hasLo ? std::max(loMax, val - 1) : (val - 1);
                hasLo = true;
            } else if (op == BoundBinaryOperatorKind::LessOrEquals) {
                loMax = hasLo ? std::max(loMax, val) : val;
                hasLo = true;
            } else if (op == BoundBinaryOperatorKind::Greater && val < std::numeric_limits<std::int64_t>::max()) {
                hiMin = hasHi ? std::min(hiMin, val + 1) : (val + 1);
                hasHi = true;
            } else if (op == BoundBinaryOperatorKind::GreaterOrEquals) {
                hiMin = hasHi ? std::min(hiMin, val) : val;
                hasHi = true;
            }
        }
        if (hasLo && hasHi && (loMax >= hiMin || loMax + 1 >= hiMin)) {
            exhaustiveByCases = true;
        }
    }
    if (!exhaustiveByCases && isFloatingPointType(result->expression->type) && !floatRelArms.empty()) {
        for (const auto& [op1, v1] : floatRelArms) {
            for (const auto& [op2, v2] : floatRelArms) {
                const bool leftCoversBelow =
                    (op1 == BoundBinaryOperatorKind::Less || op1 == BoundBinaryOperatorKind::LessOrEquals);
                const bool rightCoversAbove =
                    (op2 == BoundBinaryOperatorKind::Greater || op2 == BoundBinaryOperatorKind::GreaterOrEquals);
                if (leftCoversBelow && rightCoversAbove) {
                    if (v1 > v2 ||
                        (v1 == v2 && (op1 == BoundBinaryOperatorKind::LessOrEquals ||
                                      op2 == BoundBinaryOperatorKind::GreaterOrEquals))) {
                        exhaustiveByCases = true;
                    }
                }
            }
        }
    }
    if (!hasCatchAll && !exhaustiveByCases) {
        diagnostics_.report(
            "RS8922", "switch expression requires an exhaustive discard arm",
            syntaxTree.span());
    }
    if (commonType == PrimitiveType::Error) return makeError(syntaxTree.span());
    result->type = commonType;
    result->typeName = commonTypeName;
    for (auto& arm : result->arms) {
        arm.value = convertExpression(
            std::move(arm.value), commonType, arm.span,
            "switch expression arm", commonTypeName);
    }
    return result;
}


} // namespace realscript::semantic
