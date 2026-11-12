#include "realscript/syntax/Syntax.h"

#include <algorithm>

namespace realscript::syntax {
namespace {

bool isIdentifierLike(SyntaxKind kind) noexcept {
    return kind == SyntaxKind::IdentifierToken || isPrimitiveTypeKeyword(kind);
}

std::optional<SyntaxToken> findModifier(
    const std::vector<SyntaxToken>& modifiers,
    SyntaxKind kind) {
    for (const auto& modifier : modifiers) {
        if (modifier.kind == kind) return modifier;
    }
    return std::nullopt;
}

} // namespace

Parser::Parser(const text::SourceText& source, diagnostics::DiagnosticBag& diagnostics)
    : source_(source), diagnostics_(diagnostics) {
    Lexer lexer(source_, diagnostics_);
    tokens_ = lexer.lexAll();
}

const SyntaxToken& Parser::current() const noexcept {
    return peek(0);
}

const SyntaxToken& Parser::peek(std::size_t offset) const noexcept {
    const auto index = position_ + offset;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
}

SyntaxToken Parser::nextToken() {
    const auto token = current();
    if (position_ < tokens_.size()) {
        ++position_;
    }
    return token;
}

SyntaxToken Parser::match(SyntaxKind expected) {
    if (current().kind == expected) {
        return nextToken();
    }
    diagnostics_.report(
        "RS1100",
        std::string("expected ") + syntaxKindName(expected) + ", found " + syntaxKindName(current().kind),
        current().span);
    return {expected, {current().span.start, 0}, "", {}, true};
}

CompilationUnitSyntax Parser::parseCompilationUnit() {
    CompilationUnitSyntax result;
    auto leadingAttributes = parseAttributeLists();
    if (current().kind == SyntaxKind::ModuleKeyword) {
        result.moduleDeclaration = parseModuleDeclaration();
        result.moduleDeclaration->attributes = std::move(leadingAttributes);
        leadingAttributes.clear();
    }
    while (current().kind == SyntaxKind::ImportKeyword) {
        result.imports.push_back(parseImportDeclaration());
    }
    while (current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        auto attributes = std::move(leadingAttributes);
        leadingAttributes.clear();
        auto moreAttributes = parseAttributeLists();
        attributes.insert(
            attributes.end(),
            std::make_move_iterator(moreAttributes.begin()),
            std::make_move_iterator(moreAttributes.end()));
        auto modifiers = parseModifiers();
        if (current().kind == SyntaxKind::ClassKeyword) {
            result.classes.push_back(parseClassDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else if (current().kind == SyntaxKind::StructKeyword) {
            result.structs.push_back(parseStructDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else if (current().kind == SyntaxKind::EnumKeyword) {
            result.enums.push_back(parseEnumDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else if (current().kind == SyntaxKind::InterfaceKeyword) {
            result.interfaces.push_back(parseInterfaceDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else if (current().kind == SyntaxKind::DelegateKeyword) {
            result.delegates.push_back(parseDelegateDeclaration(
                std::move(attributes), std::move(modifiers)));
        } else {
            result.functions.push_back(parseFunctionDeclaration(
                std::move(modifiers), std::nullopt, std::nullopt,
                std::move(attributes)));
        }
        if (position_ == before) {
            diagnostics_.report("RS1101", "parser made no progress while reading a member", current().span);
            nextToken();
        }
    }
    result.endOfFileToken = match(SyntaxKind::EndOfFileToken);
    return result;
}

std::unique_ptr<ModuleDeclarationSyntax> Parser::parseModuleDeclaration() {
    auto result = std::make_unique<ModuleDeclarationSyntax>();
    result->moduleKeyword = match(SyntaxKind::ModuleKeyword);
    parseQualifiedName(result->nameParts, result->dotTokens);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

ImportDeclarationSyntax Parser::parseImportDeclaration() {
    ImportDeclarationSyntax result;
    result.importKeyword = match(SyntaxKind::ImportKeyword);
    parseQualifiedName(result.nameParts, result.dotTokens);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

void Parser::parseQualifiedName(
    std::vector<SyntaxToken>& nameParts,
    std::vector<SyntaxToken>& dotTokens) {
    nameParts.push_back(match(SyntaxKind::IdentifierToken));
    while (current().kind == SyntaxKind::DotToken) {
        dotTokens.push_back(nextToken());
        nameParts.push_back(match(SyntaxKind::IdentifierToken));
    }
}

std::vector<SyntaxToken> Parser::parseModifiers() {
    std::vector<SyntaxToken> result;
    while (isDeclarationModifier(current().kind)) {
        auto modifier = nextToken();
        if (std::any_of(result.begin(), result.end(),
                [&](const auto& existing) {
                    return existing.kind == modifier.kind;
                })) {
            diagnostics_.report(
                "RS1116",
                "duplicate declaration modifier '" + modifier.text + "'",
                modifier.span);
        }
        result.push_back(std::move(modifier));
    }
    return result;
}

std::vector<AttributeListSyntax> Parser::parseAttributeLists() {
    std::vector<AttributeListSyntax> result;
    while (current().kind == SyntaxKind::OpenBracketToken) {
        result.push_back(parseAttributeList());
    }
    return result;
}

AttributeArgumentSyntax Parser::parseAttributeArgument() {
    AttributeArgumentSyntax result;
    if (current().kind == SyntaxKind::IdentifierToken &&
        (peek(1).kind == SyntaxKind::EqualsToken ||
         peek(1).kind == SyntaxKind::ColonToken)) {
        result.nameToken = nextToken();
        result.equalsToken = nextToken();
    }
    int parens = 0;
    int brackets = 0;
    int braces = 0;
    while (current().kind != SyntaxKind::EndOfFileToken) {
        if (parens == 0 && brackets == 0 && braces == 0 &&
            (current().kind == SyntaxKind::CommaToken ||
             current().kind == SyntaxKind::CloseParenToken)) {
            break;
        }
        if (current().kind == SyntaxKind::OpenParenToken) ++parens;
        else if (current().kind == SyntaxKind::CloseParenToken &&
                 parens > 0) --parens;
        else if (current().kind == SyntaxKind::OpenBracketToken) ++brackets;
        else if (current().kind == SyntaxKind::CloseBracketToken &&
                 brackets > 0) --brackets;
        else if (current().kind == SyntaxKind::OpenBraceToken) ++braces;
        else if (current().kind == SyntaxKind::CloseBraceToken &&
                 braces > 0) --braces;
        result.valueTokens.push_back(nextToken());
    }
    if (result.valueTokens.empty()) {
        diagnostics_.report(
            "RS1112",
            "attribute argument requires a constant value",
            current().span);
    }
    return result;
}

AttributeSyntax Parser::parseAttribute() {
    AttributeSyntax result;
    result.nameToken = match(SyntaxKind::IdentifierToken);
    if (current().kind == SyntaxKind::OpenParenToken) {
        result.openParenToken = nextToken();
        if (current().kind != SyntaxKind::CloseParenToken &&
            current().kind != SyntaxKind::EndOfFileToken) {
            result.arguments.push_back(parseAttributeArgument());
            while (current().kind == SyntaxKind::CommaToken) {
                result.commaTokens.push_back(nextToken());
                result.arguments.push_back(parseAttributeArgument());
            }
        }
        result.closeParenToken = match(SyntaxKind::CloseParenToken);
    }
    return result;
}

AttributeListSyntax Parser::parseAttributeList() {
    AttributeListSyntax result;
    result.openBracketToken = match(SyntaxKind::OpenBracketToken);
    if (current().kind != SyntaxKind::CloseBracketToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.attributes.push_back(parseAttribute());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.attributes.push_back(parseAttribute());
        }
    }
    result.closeBracketToken = match(SyntaxKind::CloseBracketToken);
    return result;
}

void Parser::parseTypeArgumentList(
    std::optional<SyntaxToken>& lessToken,
    std::vector<TypeSyntax>& arguments,
    std::vector<SyntaxToken>& commaTokens,
    std::optional<SyntaxToken>& greaterToken) {
    if (current().kind != SyntaxKind::LessToken) return;
    lessToken = nextToken();
    if (current().kind != SyntaxKind::GreaterToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        arguments.push_back(parseType());
        while (current().kind == SyntaxKind::CommaToken) {
            commaTokens.push_back(nextToken());
            arguments.push_back(parseType());
        }
    }
    greaterToken = match(SyntaxKind::GreaterToken);
}

void Parser::parseTypeParameterList(
    std::optional<SyntaxToken>& lessToken,
    std::vector<SyntaxToken>& parameters,
    std::vector<SyntaxToken>& commaTokens,
    std::optional<SyntaxToken>& greaterToken) {
    if (current().kind != SyntaxKind::LessToken) return;
    lessToken = nextToken();
    if (current().kind != SyntaxKind::GreaterToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        parameters.push_back(match(SyntaxKind::IdentifierToken));
        while (current().kind == SyntaxKind::CommaToken) {
            commaTokens.push_back(nextToken());
            parameters.push_back(match(SyntaxKind::IdentifierToken));
        }
    }
    greaterToken = match(SyntaxKind::GreaterToken);
}

void Parser::parseGenericConstraintClauses(
    std::vector<GenericConstraintClauseSyntax>& clauses) {
    while (current().kind == SyntaxKind::IdentifierToken &&
           current().text == "where") {
        GenericConstraintClauseSyntax clause;
        clause.whereKeyword = nextToken();
        clause.typeParameter = match(SyntaxKind::IdentifierToken);
        clause.colonToken = match(SyntaxKind::ColonToken);
        while (current().kind != SyntaxKind::EndOfFileToken &&
               current().kind != SyntaxKind::OpenBraceToken &&
               current().kind != SyntaxKind::SemicolonToken &&
               !(current().kind == SyntaxKind::IdentifierToken &&
                 current().text == "where")) {
            clause.constraints.push_back(nextToken());
        }
        if (clause.constraints.empty()) {
            diagnostics_.report(
                "RS1114",
                "generic constraint clause requires at least one constraint",
                clause.colonToken.span);
        }
        clauses.push_back(std::move(clause));
    }
}

TypeSyntax Parser::parseType() {
    TypeSyntax result;
    if (moduleQualifiedIdentLength(0) > 0) {
        result.name = consumeModuleQualifiedIdentifier();
    } else if (isIdentifierLike(current().kind)) {
        result.name = nextToken();
    } else {
        result.name = match(SyntaxKind::IdentifierToken);
    }
    parseTypeArgumentList(
        result.lessToken,
        result.typeArguments,
        result.typeArgumentCommaTokens,
        result.greaterToken);
    if (current().kind == SyntaxKind::QuestionToken) {
        auto underlying = std::move(result);
        const auto question = nextToken();
        result = TypeSyntax{};
        result.name.kind = SyntaxKind::IdentifierToken;
        result.name.text = "Nullable";
        result.name.span = underlying.name.span;
        result.name.missing = true;
        SyntaxToken less;
        less.kind = SyntaxKind::LessToken;
        less.span = question.span;
        less.text = "<";
        less.missing = true;
        SyntaxToken greater = less;
        greater.kind = SyntaxKind::GreaterToken;
        greater.text = ">";
        result.lessToken = less;
        result.typeArguments.push_back(std::move(underlying));
        result.greaterToken = greater;
        result.nullableToken = question;
    }
    if (current().kind == SyntaxKind::OpenBracketToken &&
        peek(1).kind == SyntaxKind::CloseBracketToken) {
        result.openBracketToken = nextToken();
        result.closeBracketToken = nextToken();
        bool reportedJagged = false;
        while (current().kind == SyntaxKind::OpenBracketToken &&
               peek(1).kind == SyntaxKind::CloseBracketToken) {
            const auto extraOpen = nextToken();
            const auto extraClose = nextToken();
            if (!reportedJagged) {
                diagnostics_.report(
                    "RS8511",
                    "jagged / nested array type (rank > 1) is not supported",
                    text::TextSpan::fromBounds(extraOpen.span.start, extraClose.span.end()));
                reportedJagged = true;
            }
        }
    }
    return result;
}

ParameterSyntax Parser::parseParameter() {
    ParameterSyntax result;
    result.attributes = parseAttributeLists();
    if (current().kind == SyntaxKind::ParamsKeyword) {
        result.paramsKeyword = nextToken();
    }
    for (int modStep = 0; modStep < 2; ++modStep) {
        if (!result.thisKeyword && current().kind == SyntaxKind::ThisKeyword) {
            result.thisKeyword = nextToken();
            continue;
        }
        if (!result.modifierToken &&
            (current().kind == SyntaxKind::RefKeyword ||
             current().kind == SyntaxKind::OutKeyword ||
             current().kind == SyntaxKind::InKeyword)) {
            result.modifierToken = nextToken();
            continue;
        }
        break;
    }
    result.type = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    if (current().kind == SyntaxKind::EqualsToken) {
        result.equalsToken = nextToken();
        result.defaultValue = parseExpression();
    }
    return result;
}

FieldDeclarationSyntax Parser::parseFieldDeclaration(
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    FieldDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.type = std::move(type);
    result.identifierToken = std::move(identifier);
    if (current().kind == SyntaxKind::EqualsToken) {
        result.equalsToken = nextToken();
        if (result.type.isArray() && current().kind == SyntaxKind::OpenBraceToken) {
            result.initializer = parseBraceArrayInitializer(result.type);
        } else {
            result.initializer = parseExpression();
        }
    }
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

void Parser::parseArgumentList(
    std::vector<std::unique_ptr<ExpressionSyntax>>& arguments,
    std::vector<std::optional<SyntaxToken>>* argumentModifiers,
    std::vector<SyntaxToken>& commaTokens,
    SyntaxToken& closeParenToken,
    std::vector<std::optional<SyntaxToken>>* argumentNames,
    std::vector<std::optional<SyntaxToken>>* argumentNameColonTokens) {
    const auto parseArgument = [&]() {
        std::optional<SyntaxToken> name;
        std::optional<SyntaxToken> colon;
        if (current().kind == SyntaxKind::IdentifierToken &&
            peek(1).kind == SyntaxKind::ColonToken) {
            name = nextToken();
            colon = nextToken();
        }
        if (argumentNames) argumentNames->push_back(name);
        if (argumentNameColonTokens) {
            argumentNameColonTokens->push_back(colon);
        }
        std::optional<SyntaxToken> modifier;
        if (current().kind == SyntaxKind::RefKeyword ||
            current().kind == SyntaxKind::OutKeyword ||
            current().kind == SyntaxKind::InKeyword) {
            modifier = nextToken();
        }
        if (argumentModifiers) argumentModifiers->push_back(modifier);
        arguments.push_back(parseExpression());
    };
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        parseArgument();
        while (current().kind == SyntaxKind::CommaToken) {
            commaTokens.push_back(nextToken());
            parseArgument();
        }
    }
    closeParenToken = match(SyntaxKind::CloseParenToken);
}

SequenceDeclarationSyntax Parser::parseSequenceDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    SequenceDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.sequenceKeyword = match(SyntaxKind::SequenceKeyword);
    if (current().kind == SyntaxKind::LessToken) {
        result.resultLessToken = nextToken();
        result.resultType =
            std::make_unique<TypeSyntax>(parseType());
        result.resultGreaterToken = match(SyntaxKind::GreaterToken);
    }
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.parameters.push_back(parseParameter());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.parameters.push_back(parseParameter());
        }
    }
    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    result.body = parseBlockStatement();
    return result;
}

DelegateDeclarationSyntax Parser::parseDelegateDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    DelegateDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.delegateKeyword = match(SyntaxKind::DelegateKeyword);
    result.returnType = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.parameters.push_back(parseParameter());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.parameters.push_back(parseParameter());
        }
    }
    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    parseGenericConstraintClauses(result.constraints);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

EventDeclarationSyntax Parser::parseEventDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    EventDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.eventKeyword = match(SyntaxKind::EventKeyword);
    result.delegateType = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    ConstructorDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.parameters.push_back(parseParameter());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.parameters.push_back(parseParameter());
        }
    }
    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    if (current().kind == SyntaxKind::ColonToken) {
        result.initializerColonToken = nextToken();
        result.baseKeyword = match(SyntaxKind::BaseKeyword);
        result.initializerOpenParenToken = match(SyntaxKind::OpenParenToken);
        SyntaxToken close;
        parseArgumentList(
            result.baseArguments, nullptr,
            result.baseArgumentCommaTokens, close);
        result.initializerCloseParenToken = std::move(close);
    }
    result.body = parseBlockStatement();
    return result;
}

AccessorDeclarationSyntax Parser::parseAccessorDeclaration() {
    AccessorDeclarationSyntax result;
    if (current().kind == SyntaxKind::GetKeyword ||
        current().kind == SyntaxKind::SetKeyword) {
        result.keyword = nextToken();
    } else {
        result.keyword = match(SyntaxKind::GetKeyword);
    }
    if (current().kind == SyntaxKind::SemicolonToken) {
        result.semicolonToken = nextToken();
    } else {
        result.body = std::make_unique<BlockStatementSyntax>(parseBlockStatement());
    }
    return result;
}

PropertyDeclarationSyntax Parser::parsePropertyDeclaration(
    std::vector<SyntaxToken> modifiers,
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes) {
    PropertyDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.staticKeyword = findModifier(result.modifiers, SyntaxKind::StaticKeyword);
    result.type = std::move(type);
    result.identifierToken = std::move(identifier);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        auto accessor = std::make_unique<AccessorDeclarationSyntax>(
            parseAccessorDeclaration());
        if (accessor->keyword.kind == SyntaxKind::GetKeyword) {
            if (result.getter) {
                diagnostics_.report("RS1104", "duplicate get accessor", accessor->keyword.span);
            } else {
                result.getter = std::move(accessor);
            }
        } else {
            if (result.setter) {
                diagnostics_.report("RS1105", "duplicate set accessor", accessor->keyword.span);
            } else {
                result.setter = std::move(accessor);
            }
        }
    }
    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    if (current().kind == SyntaxKind::EqualsToken) {
        result.equalsToken = nextToken();
        if (result.type.isArray() && current().kind == SyntaxKind::OpenBraceToken) {
            result.initializer = parseBraceArrayInitializer(result.type);
        } else {
            result.initializer = parseExpression();
        }
        result.semicolonToken = match(SyntaxKind::SemicolonToken);
    } else if (current().kind == SyntaxKind::SemicolonToken) {
        result.semicolonToken = nextToken();
    }
    return result;
}

FunctionDeclarationSyntax Parser::parseFunctionDeclaration(
    std::vector<SyntaxToken> modifiers,
    std::optional<TypeSyntax> returnType,
    std::optional<SyntaxToken> identifier,
    std::vector<AttributeListSyntax> attributes) {
    FunctionDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.staticKeyword = findModifier(result.modifiers, SyntaxKind::StaticKeyword);
    if (!returnType && current().kind == SyntaxKind::RefKeyword) {
        result.refReturnKeyword = nextToken();
    }
    result.returnType = returnType ? std::move(*returnType) : parseType();
    result.identifierToken = identifier ? std::move(*identifier) :
        match(SyntaxKind::IdentifierToken);
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);

    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.parameters.push_back(parseParameter());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.parameters.push_back(parseParameter());
        }
    }

    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    parseGenericConstraintClauses(result.constraints);
    if (current().kind == SyntaxKind::SemicolonToken) {
        result.semicolonToken = nextToken();
    } else {
        result.body = parseBlockStatement();
    }
    return result;
}

ClassDeclarationSyntax Parser::parseClassDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    ClassDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.classKeyword = match(SyntaxKind::ClassKeyword);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    parseInterfaceList(
        result.colonToken,
        result.interfaces,
        result.interfaceCommaTokens);
    parseGenericConstraintClauses(result.constraints);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        auto memberAttributes = parseAttributeLists();
        auto memberModifiers = parseModifiers();
        if (current().kind == SyntaxKind::EventKeyword) {
            result.events.push_back(parseEventDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
            continue;
        }
        const auto isStatic = findModifier(
            memberModifiers, SyntaxKind::StaticKeyword).has_value();
        if (!isStatic && current().kind == SyntaxKind::SequenceKeyword) {
            result.sequences.push_back(parseSequenceDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
        } else if (current().kind == SyntaxKind::IdentifierToken &&
            current().text == typeName && peek(1).kind == SyntaxKind::OpenParenToken) {
            result.constructors.push_back(parseConstructorDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
        } else {
            if (current().kind == SyntaxKind::IdentifierToken &&
                (current().text == "implicit" || current().text == "explicit") &&
                peek(1).kind == SyntaxKind::OperatorKeyword) {
                const auto convKeyword = nextToken();
                const auto operatorKeyword = nextToken();
                auto targetType = parseType();
                SyntaxToken identifier;
                identifier.kind = SyntaxKind::IdentifierToken;
                identifier.span = operatorKeyword.span;
                identifier.text = convKeyword.text == "implicit"
                    ? "op_Implicit"
                    : "op_Explicit";
                auto method = parseFunctionDeclaration(
                    std::move(memberModifiers), std::move(targetType),
                    std::move(identifier), std::move(memberAttributes));
                if (!isStatic) {
                    diagnostics_.report(
                        "RS1112",
                        "user-defined conversion operator declaration must be static",
                        operatorKeyword.span);
                }
                if (method.parameters.size() != 1) {
                    diagnostics_.report(
                        "RS1113",
                        "user-defined conversion operator must take exactly 1 parameter",
                        operatorKeyword.span);
                    method.identifierToken.text = "op_Invalid";
                }
                result.methods.push_back(std::move(method));
                continue;
            }
            std::optional<SyntaxToken> refReturnKeyword;
            if (current().kind == SyntaxKind::RefKeyword) {
                refReturnKeyword = nextToken();
            }
            auto type = parseType();
            if (current().kind == SyntaxKind::OperatorKeyword) {
                const auto operatorKeyword = nextToken();
                const auto opToken = nextToken();
                SyntaxToken identifier;
                identifier.kind = SyntaxKind::IdentifierToken;
                identifier.span = opToken.span;
                identifier.text = opToken.text;
                auto method = parseFunctionDeclaration(
                    std::move(memberModifiers), std::move(type),
                    std::move(identifier), std::move(memberAttributes));
                method.refReturnKeyword = refReturnKeyword;
                if (!isStatic) {
                    diagnostics_.report(
                        "RS1112",
                        "user-defined operator declaration must be static",
                        operatorKeyword.span);
                }
                std::string opName;
                const auto arity = method.parameters.size();
                switch (opToken.kind) {
                case SyntaxKind::PlusToken:
                    opName = arity == 1 ? "op_UnaryPlus" : "op_Addition";
                    if (arity != 1 && arity != 2) opName.clear();
                    break;
                case SyntaxKind::MinusToken:
                    opName = arity == 1 ? "op_UnaryNegation" : "op_Subtraction";
                    if (arity != 1 && arity != 2) opName.clear();
                    break;
                case SyntaxKind::BangToken:
                    if (arity == 1) opName = "op_LogicalNot";
                    break;
                case SyntaxKind::StarToken:
                    if (arity == 2) opName = "op_Multiply";
                    break;
                case SyntaxKind::SlashToken:
                    if (arity == 2) opName = "op_Division";
                    break;
                case SyntaxKind::PercentToken:
                    if (arity == 2) opName = "op_Modulus";
                    break;
                case SyntaxKind::EqualsEqualsToken:
                    if (arity == 2) opName = "op_Equality";
                    break;
                case SyntaxKind::BangEqualsToken:
                    if (arity == 2) opName = "op_Inequality";
                    break;
                case SyntaxKind::LessToken:
                    if (arity == 2) opName = "op_LessThan";
                    break;
                case SyntaxKind::LessOrEqualsToken:
                    if (arity == 2) opName = "op_LessThanOrEqual";
                    break;
                case SyntaxKind::GreaterToken:
                    if (arity == 2) opName = "op_GreaterThan";
                    break;
                case SyntaxKind::GreaterOrEqualsToken:
                    if (arity == 2) opName = "op_GreaterThanOrEqual";
                    break;
                default:
                    break;
                }
                if (opName.empty()) {
                    diagnostics_.report(
                        "RS1113",
                        "unsupported operator or invalid parameter count for operator '" +
                            opToken.text + "'",
                        opToken.span);
                    opName = "op_Invalid";
                }
                method.identifierToken.text = std::move(opName);
                result.methods.push_back(std::move(method));
            } else if (current().kind == SyntaxKind::ThisKeyword &&
                       peek(1).kind == SyntaxKind::OpenBracketToken) {
                const auto thisTok = nextToken();
                const auto openBracket = match(SyntaxKind::OpenBracketToken);
                std::vector<ParameterSyntax> idxParams;
                std::vector<SyntaxToken> idxCommas;
                if (current().kind != SyntaxKind::CloseBracketToken &&
                    current().kind != SyntaxKind::EndOfFileToken) {
                    idxParams.push_back(parseParameter());
                    while (current().kind == SyntaxKind::CommaToken) {
                        idxCommas.push_back(nextToken());
                        idxParams.push_back(parseParameter());
                    }
                }
                const auto closeBracket = match(SyntaxKind::CloseBracketToken);
                (void)openBracket;
                (void)closeBracket;
                (void)match(SyntaxKind::OpenBraceToken);
                while (current().kind != SyntaxKind::CloseBraceToken &&
                       current().kind != SyntaxKind::EndOfFileToken) {
                    auto accessor = parseAccessorDeclaration();
                    if (accessor.keyword.kind == SyntaxKind::GetKeyword && accessor.body) {
                        FunctionDeclarationSyntax getter;
                        getter.attributes = memberAttributes;
                        getter.modifiers = memberModifiers;
                        getter.staticKeyword = findModifier(getter.modifiers, SyntaxKind::StaticKeyword);
                        getter.refReturnKeyword = refReturnKeyword;
                        getter.returnType = type;
                        getter.identifierToken.kind = SyntaxKind::IdentifierToken;
                        getter.identifierToken.span = thisTok.span;
                        getter.identifierToken.text = "Get";
                        for (const auto& ip : idxParams) {
                            getter.parameters.push_back(cloneParameterSyntax(ip));
                        }
                        getter.commaTokens = idxCommas;
                        getter.body = std::move(*accessor.body);
                        result.methods.push_back(std::move(getter));
                    } else if (accessor.keyword.kind == SyntaxKind::SetKeyword && accessor.body) {
                        FunctionDeclarationSyntax setter;
                        setter.attributes = memberAttributes;
                        setter.modifiers = memberModifiers;
                        setter.staticKeyword = findModifier(setter.modifiers, SyntaxKind::StaticKeyword);
                        TypeSyntax voidType;
                        voidType.name.kind = SyntaxKind::VoidKeyword;
                        voidType.name.span = thisTok.span;
                        voidType.name.text = "void";
                        setter.returnType = std::move(voidType);
                        setter.identifierToken.kind = SyntaxKind::IdentifierToken;
                        setter.identifierToken.span = thisTok.span;
                        setter.identifierToken.text = "Set";
                        for (const auto& ip : idxParams) {
                            setter.parameters.push_back(cloneParameterSyntax(ip));
                        }
                        setter.commaTokens = idxCommas;
                        ParameterSyntax valParam;
                        valParam.type = type;
                        valParam.identifierToken.kind = SyntaxKind::IdentifierToken;
                        valParam.identifierToken.span = accessor.keyword.span;
                        valParam.identifierToken.text = "value";
                        setter.parameters.push_back(std::move(valParam));
                        setter.body = std::move(*accessor.body);
                        result.methods.push_back(std::move(setter));
                    }
                }
                (void)match(SyntaxKind::CloseBraceToken);
            } else {
                auto identifier = match(SyntaxKind::IdentifierToken);
                const auto isAbstract = findModifier(
                    memberModifiers, SyntaxKind::AbstractKeyword).has_value();
                if (current().kind == SyntaxKind::OpenParenToken ||
                    current().kind == SyntaxKind::LessToken ||
                    (isAbstract && current().kind == SyntaxKind::SemicolonToken)) {
                    auto method = parseFunctionDeclaration(
                        std::move(memberModifiers), std::move(type),
                        std::move(identifier), std::move(memberAttributes));
                    method.refReturnKeyword = refReturnKeyword;
                    result.methods.push_back(std::move(method));
                } else if (current().kind == SyntaxKind::OpenBraceToken) {
                    result.properties.push_back(parsePropertyDeclaration(
                        std::move(memberModifiers), std::move(type),
                        std::move(identifier), std::move(memberAttributes)));
                } else {
                    result.fields.push_back(parseFieldDeclaration(
                        std::move(type), std::move(identifier),
                        std::move(memberAttributes), std::move(memberModifiers)));
                }
            }
        }
        if (before == position_) {
            diagnostics_.report("RS1103", "parser made no progress while reading a class member", current().span);
            nextToken();
        }
    }
    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

StructDeclarationSyntax Parser::parseStructDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    StructDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.structKeyword = match(SyntaxKind::StructKeyword);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    const auto typeName = result.identifierToken.text;
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    parseInterfaceList(
        result.colonToken,
        result.interfaces,
        result.interfaceCommaTokens);
    parseGenericConstraintClauses(result.constraints);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        auto memberAttributes = parseAttributeLists();
        auto memberModifiers = parseModifiers();
        if (current().kind == SyntaxKind::EventKeyword) {
            result.events.push_back(parseEventDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
            continue;
        }
        const auto isStatic = findModifier(
            memberModifiers, SyntaxKind::StaticKeyword).has_value();
        if (!isStatic && current().kind == SyntaxKind::SequenceKeyword) {
            result.sequences.push_back(parseSequenceDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
        } else if (current().kind == SyntaxKind::IdentifierToken &&
            current().text == typeName && peek(1).kind == SyntaxKind::OpenParenToken) {
            result.constructors.push_back(parseConstructorDeclaration(
                std::move(memberAttributes), std::move(memberModifiers)));
        } else {
            if (current().kind == SyntaxKind::IdentifierToken &&
                (current().text == "implicit" || current().text == "explicit") &&
                peek(1).kind == SyntaxKind::OperatorKeyword) {
                const auto convKeyword = nextToken();
                const auto operatorKeyword = nextToken();
                auto targetType = parseType();
                SyntaxToken identifier;
                identifier.kind = SyntaxKind::IdentifierToken;
                identifier.span = operatorKeyword.span;
                identifier.text = convKeyword.text == "implicit"
                    ? "op_Implicit"
                    : "op_Explicit";
                auto method = parseFunctionDeclaration(
                    std::move(memberModifiers), std::move(targetType),
                    std::move(identifier), std::move(memberAttributes));
                if (!isStatic) {
                    diagnostics_.report(
                        "RS1112",
                        "user-defined conversion operator declaration must be static",
                        operatorKeyword.span);
                }
                if (method.parameters.size() != 1) {
                    diagnostics_.report(
                        "RS1113",
                        "user-defined conversion operator must take exactly 1 parameter",
                        operatorKeyword.span);
                    method.identifierToken.text = "op_Invalid";
                }
                result.methods.push_back(std::move(method));
                continue;
            }
            std::optional<SyntaxToken> refReturnKeyword;
            if (current().kind == SyntaxKind::RefKeyword) {
                refReturnKeyword = nextToken();
            }
            auto type = parseType();
            if (current().kind == SyntaxKind::OperatorKeyword) {
                const auto operatorKeyword = nextToken();
                const auto opToken = nextToken();
                SyntaxToken identifier;
                identifier.kind = SyntaxKind::IdentifierToken;
                identifier.span = opToken.span;
                identifier.text = opToken.text;
                auto method = parseFunctionDeclaration(
                    std::move(memberModifiers), std::move(type),
                    std::move(identifier), std::move(memberAttributes));
                method.refReturnKeyword = refReturnKeyword;
                if (!isStatic) {
                    diagnostics_.report(
                        "RS1112",
                        "user-defined operator declaration must be static",
                        operatorKeyword.span);
                }
                std::string opName;
                const auto arity = method.parameters.size();
                switch (opToken.kind) {
                case SyntaxKind::PlusToken:
                    opName = arity == 1 ? "op_UnaryPlus" : "op_Addition";
                    if (arity != 1 && arity != 2) opName.clear();
                    break;
                case SyntaxKind::MinusToken:
                    opName = arity == 1 ? "op_UnaryNegation" : "op_Subtraction";
                    if (arity != 1 && arity != 2) opName.clear();
                    break;
                case SyntaxKind::BangToken:
                    if (arity == 1) opName = "op_LogicalNot";
                    break;
                case SyntaxKind::StarToken:
                    if (arity == 2) opName = "op_Multiply";
                    break;
                case SyntaxKind::SlashToken:
                    if (arity == 2) opName = "op_Division";
                    break;
                case SyntaxKind::PercentToken:
                    if (arity == 2) opName = "op_Modulus";
                    break;
                case SyntaxKind::EqualsEqualsToken:
                    if (arity == 2) opName = "op_Equality";
                    break;
                case SyntaxKind::BangEqualsToken:
                    if (arity == 2) opName = "op_Inequality";
                    break;
                case SyntaxKind::LessToken:
                    if (arity == 2) opName = "op_LessThan";
                    break;
                case SyntaxKind::LessOrEqualsToken:
                    if (arity == 2) opName = "op_LessThanOrEqual";
                    break;
                case SyntaxKind::GreaterToken:
                    if (arity == 2) opName = "op_GreaterThan";
                    break;
                case SyntaxKind::GreaterOrEqualsToken:
                    if (arity == 2) opName = "op_GreaterThanOrEqual";
                    break;
                default:
                    break;
                }
                if (opName.empty()) {
                    diagnostics_.report(
                        "RS1113",
                        "unsupported operator or invalid parameter count for operator '" +
                            opToken.text + "'",
                        opToken.span);
                    opName = "op_Invalid";
                }
                method.identifierToken.text = std::move(opName);
                result.methods.push_back(std::move(method));
            } else if (current().kind == SyntaxKind::ThisKeyword &&
                       peek(1).kind == SyntaxKind::OpenBracketToken) {
                const auto thisTok = nextToken();
                const auto openBracket = match(SyntaxKind::OpenBracketToken);
                std::vector<ParameterSyntax> idxParams;
                std::vector<SyntaxToken> idxCommas;
                if (current().kind != SyntaxKind::CloseBracketToken &&
                    current().kind != SyntaxKind::EndOfFileToken) {
                    idxParams.push_back(parseParameter());
                    while (current().kind == SyntaxKind::CommaToken) {
                        idxCommas.push_back(nextToken());
                        idxParams.push_back(parseParameter());
                    }
                }
                const auto closeBracket = match(SyntaxKind::CloseBracketToken);
                (void)openBracket;
                (void)closeBracket;
                (void)match(SyntaxKind::OpenBraceToken);
                while (current().kind != SyntaxKind::CloseBraceToken &&
                       current().kind != SyntaxKind::EndOfFileToken) {
                    auto accessor = parseAccessorDeclaration();
                    if (accessor.keyword.kind == SyntaxKind::GetKeyword && accessor.body) {
                        FunctionDeclarationSyntax getter;
                        getter.attributes = memberAttributes;
                        getter.modifiers = memberModifiers;
                        getter.staticKeyword = findModifier(getter.modifiers, SyntaxKind::StaticKeyword);
                        getter.refReturnKeyword = refReturnKeyword;
                        getter.returnType = type;
                        getter.identifierToken.kind = SyntaxKind::IdentifierToken;
                        getter.identifierToken.span = thisTok.span;
                        getter.identifierToken.text = "Get";
                        for (const auto& ip : idxParams) {
                            getter.parameters.push_back(cloneParameterSyntax(ip));
                        }
                        getter.commaTokens = idxCommas;
                        getter.body = std::move(*accessor.body);
                        result.methods.push_back(std::move(getter));
                    } else if (accessor.keyword.kind == SyntaxKind::SetKeyword && accessor.body) {
                        FunctionDeclarationSyntax setter;
                        setter.attributes = memberAttributes;
                        setter.modifiers = memberModifiers;
                        setter.staticKeyword = findModifier(setter.modifiers, SyntaxKind::StaticKeyword);
                        TypeSyntax voidType;
                        voidType.name.kind = SyntaxKind::VoidKeyword;
                        voidType.name.span = thisTok.span;
                        voidType.name.text = "void";
                        setter.returnType = std::move(voidType);
                        setter.identifierToken.kind = SyntaxKind::IdentifierToken;
                        setter.identifierToken.span = thisTok.span;
                        setter.identifierToken.text = "Set";
                        for (const auto& ip : idxParams) {
                            setter.parameters.push_back(cloneParameterSyntax(ip));
                        }
                        setter.commaTokens = idxCommas;
                        ParameterSyntax valParam;
                        valParam.type = type;
                        valParam.identifierToken.kind = SyntaxKind::IdentifierToken;
                        valParam.identifierToken.span = accessor.keyword.span;
                        valParam.identifierToken.text = "value";
                        setter.parameters.push_back(std::move(valParam));
                        setter.body = std::move(*accessor.body);
                        result.methods.push_back(std::move(setter));
                    }
                }
                (void)match(SyntaxKind::CloseBraceToken);
            } else {
                auto identifier = match(SyntaxKind::IdentifierToken);
                const auto isAbstract = findModifier(
                    memberModifiers, SyntaxKind::AbstractKeyword).has_value();
                if (current().kind == SyntaxKind::OpenParenToken ||
                    current().kind == SyntaxKind::LessToken ||
                    (isAbstract && current().kind == SyntaxKind::SemicolonToken)) {
                    auto method = parseFunctionDeclaration(
                        std::move(memberModifiers), std::move(type),
                        std::move(identifier), std::move(memberAttributes));
                    method.refReturnKeyword = refReturnKeyword;
                    result.methods.push_back(std::move(method));
                } else if (current().kind == SyntaxKind::OpenBraceToken) {
                    result.properties.push_back(parsePropertyDeclaration(
                        std::move(memberModifiers), std::move(type),
                        std::move(identifier), std::move(memberAttributes)));
                } else {
                    result.fields.push_back(parseFieldDeclaration(
                        std::move(type), std::move(identifier),
                        std::move(memberAttributes), std::move(memberModifiers)));
                }
            }
        }
        if (before == position_) {
            diagnostics_.report("RS1103", "parser made no progress while reading a struct member", current().span);
            nextToken();
        }
    }
    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

void Parser::parseInterfaceList(
    std::optional<SyntaxToken>& colonToken,
    std::vector<TypeSyntax>& interfaces,
    std::vector<SyntaxToken>& commaTokens) {
    if (current().kind != SyntaxKind::ColonToken) return;
    colonToken = nextToken();
    interfaces.push_back(parseType());
    while (current().kind == SyntaxKind::CommaToken) {
        commaTokens.push_back(nextToken());
        interfaces.push_back(parseType());
    }
}

InterfaceMethodDeclarationSyntax Parser::parseInterfaceMethodDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    InterfaceMethodDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.returnType = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.parameters.push_back(parseParameter());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.parameters.push_back(parseParameter());
        }
    }
    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    parseGenericConstraintClauses(result.constraints);
    if (current().kind == SyntaxKind::OpenBraceToken) {
        result.body = parseBlockStatement();
    } else {
        result.semicolonToken = match(SyntaxKind::SemicolonToken);
    }
    return result;
}

InterfaceDeclarationSyntax Parser::parseInterfaceDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    InterfaceDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.interfaceKeyword = match(SyntaxKind::InterfaceKeyword);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    parseTypeParameterList(
        result.typeParameterLessToken,
        result.typeParameters,
        result.typeParameterCommaTokens,
        result.typeParameterGreaterToken);
    parseGenericConstraintClauses(result.constraints);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        auto memberAttributes = parseAttributeLists();
        auto memberModifiers = parseModifiers();
        result.methods.push_back(parseInterfaceMethodDeclaration(
            std::move(memberAttributes), std::move(memberModifiers)));
        if (before == position_) {
            diagnostics_.report(
                "RS1111",
                "parser made no progress while reading an interface member",
                current().span);
            nextToken();
        }
    }
    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

EnumDeclarationSyntax Parser::parseEnumDeclaration(
    std::vector<AttributeListSyntax> attributes,
    std::vector<SyntaxToken> modifiers) {
    EnumDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.modifiers = std::move(modifiers);
    result.enumKeyword = match(SyntaxKind::EnumKeyword);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        EnumMemberDeclarationSyntax member;
        member.attributes = parseAttributeLists();
        member.identifierToken = match(SyntaxKind::IdentifierToken);
        if (current().kind == SyntaxKind::EqualsToken) {
            member.equalsToken = nextToken();
            if (current().kind == SyntaxKind::MinusToken) {
                const auto minus = nextToken();
                auto value = match(SyntaxKind::IntegerLiteralToken);
                value.text = "-" + value.text;
                if (std::holds_alternative<std::int64_t>(value.value)) {
                    value.value = -std::get<std::int64_t>(value.value);
                }
                value.span = text::TextSpan::fromBounds(minus.span.start, value.span.end());
                member.valueToken = std::move(value);
            } else {
                member.valueToken = match(SyntaxKind::IntegerLiteralToken);
            }
        }
        if (current().kind == SyntaxKind::CommaToken) member.commaToken = nextToken();
        result.members.push_back(std::move(member));
        if (!result.members.back().commaToken && current().kind != SyntaxKind::CloseBraceToken) {
            diagnostics_.report("RS1107", "expected comma between enum members", current().span);
        }
    }
    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

std::unique_ptr<TryStatementSyntax> Parser::buildUsingTryStatement(
    const SyntaxToken& usingToken,
    const SyntaxToken& resourceToken,
    BlockStatementSyntax tryBody,
    text::TextSpan endSpan) const {
    auto disposeCall = std::make_unique<MemberCallExpressionSyntax>();
    auto receiver = std::make_unique<NameExpressionSyntax>();
    receiver->identifierToken = resourceToken;
    disposeCall->receiver = std::move(receiver);
    disposeCall->dotToken = {
        SyntaxKind::DotToken, resourceToken.span, ".", {}, false};
    disposeCall->nameToken = {
        SyntaxKind::IdentifierToken, resourceToken.span, "Dispose", {}, false};
    disposeCall->openParenToken = {
        SyntaxKind::OpenParenToken, resourceToken.span, "(", {}, false};
    disposeCall->closeParenToken = {
        SyntaxKind::CloseParenToken, resourceToken.span, ")", {}, false};

    auto disposeStmt = std::make_unique<ExpressionStatementSyntax>();
    disposeStmt->expression = std::move(disposeCall);
    disposeStmt->semicolonToken = {
        SyntaxKind::SemicolonToken, resourceToken.span, ";", {}, false};

    BlockStatementSyntax finallyBlock;
    finallyBlock.openBraceToken = {
        SyntaxKind::OpenBraceToken, endSpan, "{", {}, false};
    finallyBlock.statements.push_back(std::move(disposeStmt));
    finallyBlock.closeBraceToken = {
        SyntaxKind::CloseBraceToken, endSpan, "}", {}, false};

    auto tryStmt = std::make_unique<TryStatementSyntax>();
    tryStmt->tryKeyword = {
        SyntaxKind::TryKeyword, usingToken.span, "try", {}, false};
    tryStmt->body = std::move(tryBody);
    tryStmt->finallyKeyword = {
        SyntaxKind::FinallyKeyword, endSpan, "$using_finally", {}, false};
    tryStmt->finallyBody = std::move(finallyBlock);
    return tryStmt;
}

void Parser::parseStatementSequence(
    std::vector<std::unique_ptr<StatementSyntax>>& statements,
    bool stopAtSwitchLabel) {
    const auto atSequenceEnd = [&]() noexcept {
        const auto kind = current().kind;
        if (kind == SyntaxKind::CloseBraceToken ||
            kind == SyntaxKind::EndOfFileToken) {
            return true;
        }
        if (stopAtSwitchLabel &&
            (kind == SyntaxKind::CaseKeyword ||
             kind == SyntaxKind::DefaultKeyword)) {
            return true;
        }
        return false;
    };

    while (!atSequenceEnd()) {
        if (isUsingDeclarationStart()) {
            const auto usingToken = nextToken();
            auto declaredType = parseType();
            std::vector<std::unique_ptr<VariableDeclarationStatementSyntax>> decls;
            do {
                auto varDecl = std::make_unique<VariableDeclarationStatementSyntax>();
                varDecl->isUsingDeclaration = true;
                varDecl->type = declaredType;
                varDecl->identifierToken = match(SyntaxKind::IdentifierToken);
                if (current().kind == SyntaxKind::EqualsToken) {
                    varDecl->equalsToken = nextToken();
                    varDecl->initializer = parseExpression();
                } else {
                    diagnostics_.report(
                        "RS1125",
                        "using declaration requires an initializer",
                        varDecl->identifierToken.span);
                }
                decls.push_back(std::move(varDecl));
            } while (current().kind == SyntaxKind::CommaToken &&
                     (nextToken(), true));
            const auto semiToken = match(SyntaxKind::SemicolonToken);
            for (auto& d : decls) {
                d->semicolonToken = semiToken;
            }

            BlockStatementSyntax restBlock;
            restBlock.openBraceToken = {
                SyntaxKind::OpenBraceToken, semiToken.span, "{", {}, false};
            parseStatementSequence(restBlock.statements, stopAtSwitchLabel);
            const auto tailSpan = current().span.empty() ? semiToken.span : current().span;
            restBlock.closeBraceToken = {
                SyntaxKind::CloseBraceToken, tailSpan, "}", {}, false};

            BlockStatementSyntax currentBody = std::move(restBlock);
            for (std::size_t idx = decls.size(); idx > 0; --idx) {
                auto& decl = decls[idx - 1];
                const auto varToken = decl->identifierToken;
                auto wrappedTry = buildUsingTryStatement(
                    usingToken, varToken, std::move(currentBody), tailSpan);
                if (idx == 1) {
                    statements.push_back(std::move(decl));
                    statements.push_back(std::move(wrappedTry));
                } else {
                    BlockStatementSyntax nextBody;
                    nextBody.openBraceToken = {
                        SyntaxKind::OpenBraceToken, varToken.span, "{", {}, false};
                    nextBody.statements.push_back(std::move(decl));
                    nextBody.statements.push_back(std::move(wrappedTry));
                    nextBody.closeBraceToken = {
                        SyntaxKind::CloseBraceToken, tailSpan, "}", {}, false};
                    currentBody = std::move(nextBody);
                }
            }
            return;
        }

        const auto before = position_;
        statements.push_back(parseStatement());
        if (before == position_) {
            diagnostics_.report("RS1102", "parser made no progress while reading a statement", current().span);
            nextToken();
        }
    }
}

BlockStatementSyntax Parser::parseBlockStatement() {
    BlockStatementSyntax result;
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    parseStatementSequence(result.statements, false);
    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseStatement() {
    if (current().kind == SyntaxKind::IdentifierToken &&
        current().text == "using" &&
        peek(1).kind == SyntaxKind::OpenParenToken) {
        const auto usingToken = nextToken();
        const auto openParen = match(SyntaxKind::OpenParenToken);
        (void)openParen;
        std::vector<std::unique_ptr<VariableDeclarationStatementSyntax>> decls;
        if (isVariableDeclarationStartAt(0, true)) {
            const auto declaredType = parseType();
            do {
                auto varDecl = std::make_unique<VariableDeclarationStatementSyntax>();
                varDecl->isUsingDeclaration = true;
                varDecl->type = declaredType;
                varDecl->identifierToken = match(SyntaxKind::IdentifierToken);
                varDecl->equalsToken = match(SyntaxKind::EqualsToken);
                varDecl->initializer = parseExpression();
                decls.push_back(std::move(varDecl));
            } while (current().kind == SyntaxKind::CommaToken &&
                     (nextToken(), true));
        } else {
            const auto exprPos = position_;
            auto resourceExpr = parseExpression();
            const auto exprSpan = resourceExpr ? resourceExpr->span() : usingToken.span;
            auto varDecl = std::make_unique<VariableDeclarationStatementSyntax>();
            varDecl->isUsingDeclaration = true;
            varDecl->type.name = {
                SyntaxKind::IdentifierToken, exprSpan, "var", {}, false};
            varDecl->identifierToken = {
                SyntaxKind::IdentifierToken,
                exprSpan,
                "$using_expr_" + std::to_string(exprPos),
                {},
                false};
            varDecl->equalsToken = {
                SyntaxKind::EqualsToken, exprSpan, "=", {}, false};
            varDecl->initializer = std::move(resourceExpr);
            decls.push_back(std::move(varDecl));
        }
        const auto closeParen = match(SyntaxKind::CloseParenToken);
        for (auto& d : decls) {
            d->semicolonToken = {
                SyntaxKind::SemicolonToken, closeParen.span, ";", {}, false};
        }

        auto innerBody = parseStatement();
        BlockStatementSyntax currentBody;
        if (innerBody && innerBody->kind() == SyntaxKind::BlockStatement) {
            currentBody = std::move(static_cast<BlockStatementSyntax&>(*innerBody));
        } else {
            currentBody.openBraceToken = {
                SyntaxKind::OpenBraceToken, usingToken.span, "{", {}, false};
            if (innerBody) {
                currentBody.statements.push_back(std::move(innerBody));
            }
            currentBody.closeBraceToken = {
                SyntaxKind::CloseBraceToken, closeParen.span, "}", {}, false};
        }

        const auto endSpan = currentBody.closeBraceToken.span.empty()
            ? closeParen.span
            : currentBody.closeBraceToken.span;
        for (std::size_t idx = decls.size(); idx > 0; --idx) {
            auto& decl = decls[idx - 1];
            const auto varToken = decl->identifierToken;
            auto tryStmt = buildUsingTryStatement(
                usingToken, varToken, std::move(currentBody), endSpan);
            BlockStatementSyntax outerBlock;
            outerBlock.openBraceToken = {
                SyntaxKind::OpenBraceToken, usingToken.span, "{", {}, false};
            outerBlock.statements.push_back(std::move(decl));
            outerBlock.statements.push_back(std::move(tryStmt));
            outerBlock.closeBraceToken = {
                SyntaxKind::CloseBraceToken, endSpan, "}", {}, false};
            currentBody = std::move(outerBlock);
        }
        return std::make_unique<BlockStatementSyntax>(std::move(currentBody));
    }
    if (isUsingDeclarationStart()) {
        diagnostics_.report(
            "RS1126",
            "embedded statement cannot be a 'using' declaration; wrap it in a '{ ... }' block",
            current().span);
        BlockStatementSyntax immediateBlock;
        immediateBlock.openBraceToken = {
            SyntaxKind::OpenBraceToken, current().span, "{", {}, false};
        const auto usingToken = nextToken();
        const auto declaredType = parseType();
        std::vector<std::unique_ptr<VariableDeclarationStatementSyntax>> decls;
        do {
            auto varDecl = std::make_unique<VariableDeclarationStatementSyntax>();
            varDecl->isUsingDeclaration = true;
            varDecl->type = declaredType;
            varDecl->identifierToken = match(SyntaxKind::IdentifierToken);
            if (current().kind == SyntaxKind::EqualsToken) {
                varDecl->equalsToken = nextToken();
                varDecl->initializer = parseExpression();
            } else {
                diagnostics_.report(
                    "RS1125",
                    "using declaration requires an initializer",
                    varDecl->identifierToken.span);
            }
            decls.push_back(std::move(varDecl));
        } while (current().kind == SyntaxKind::CommaToken &&
                 (nextToken(), true));
        const auto semiToken = match(SyntaxKind::SemicolonToken);
        for (auto& d : decls) {
            d->semicolonToken = semiToken;
        }
        BlockStatementSyntax currentBody;
        currentBody.openBraceToken = {
            SyntaxKind::OpenBraceToken, semiToken.span, "{", {}, false};
        currentBody.closeBraceToken = {
            SyntaxKind::CloseBraceToken, semiToken.span, "}", {}, false};
        for (std::size_t idx = decls.size(); idx > 0; --idx) {
            auto& decl = decls[idx - 1];
            const auto varToken = decl->identifierToken;
            auto wrappedTry = buildUsingTryStatement(
                usingToken, varToken, std::move(currentBody), semiToken.span);
            BlockStatementSyntax nextBody;
            nextBody.openBraceToken = {
                SyntaxKind::OpenBraceToken, usingToken.span, "{", {}, false};
            nextBody.statements.push_back(std::move(decl));
            nextBody.statements.push_back(std::move(wrappedTry));
            nextBody.closeBraceToken = {
                SyntaxKind::CloseBraceToken, semiToken.span, "}", {}, false};
            currentBody = std::move(nextBody);
        }
        immediateBlock = std::move(currentBody);
        return std::make_unique<BlockStatementSyntax>(std::move(immediateBlock));
    }
    if (current().kind == SyntaxKind::IdentifierToken &&
        (peek(1).kind == SyntaxKind::PlusEqualsToken ||
         peek(1).kind == SyntaxKind::MinusEqualsToken)) {
        return parseEventSubscriptionStatement();
    }
    if ((current().kind == SyntaxKind::IdentifierToken ||
         current().kind == SyntaxKind::ThisKeyword) &&
        peek(1).kind == SyntaxKind::DotToken &&
        peek(2).kind == SyntaxKind::IdentifierToken &&
        (peek(3).kind == SyntaxKind::PlusEqualsToken ||
         peek(3).kind == SyntaxKind::MinusEqualsToken)) {
        return parseEventSubscriptionStatement();
    }
    switch (current().kind) {
    case SyntaxKind::ReturnKeyword:
        return parseReturnStatement();
    case SyntaxKind::IfKeyword:
        return parseIfStatement();
    case SyntaxKind::WhileKeyword:
        return parseWhileStatement();
    case SyntaxKind::ForKeyword:
        return parseForStatement();
    case SyntaxKind::ForeachKeyword:
        return parseForeachStatement();
    case SyntaxKind::DoKeyword:
        return parseDoWhileStatement();
    case SyntaxKind::BreakKeyword:
        return parseBreakStatement();
    case SyntaxKind::ContinueKeyword:
        return parseContinueStatement();
    case SyntaxKind::SwitchKeyword:
        return parseSwitchStatement();
    case SyntaxKind::ThrowKeyword:
        return parseThrowStatement();
    case SyntaxKind::TryKeyword:
        return parseTryStatement();
    case SyntaxKind::YieldKeyword:
        return parseYieldStatement();
    case SyntaxKind::OpenBraceToken:
        return std::make_unique<BlockStatementSyntax>(parseBlockStatement());
    default:
        break;
    }
    if (isVariableDeclarationStart()) {
        return parseVariableDeclarationStatement();
    }
    return parseExpressionStatement();
}

std::unique_ptr<StatementSyntax> Parser::parseReturnStatement() {
    auto result = std::make_unique<ReturnStatementSyntax>();
    result->returnKeyword = match(SyntaxKind::ReturnKeyword);
    if (current().kind == SyntaxKind::RefKeyword) {
        result->refKeyword = nextToken();
    }
    if (current().kind != SyntaxKind::SemicolonToken) {
        result->expression = parseExpression();
    }
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseIfStatement() {
    auto result = std::make_unique<IfStatementSyntax>();
    result->ifKeyword = match(SyntaxKind::IfKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->condition = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->thenStatement = parseStatement();
    if (current().kind == SyntaxKind::ElseKeyword) {
        result->elseKeyword = nextToken();
        result->elseStatement = parseStatement();
    }
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseWhileStatement() {
    auto result = std::make_unique<WhileStatementSyntax>();
    result->whileKeyword = match(SyntaxKind::WhileKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->condition = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->body = parseStatement();
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseForStatement() {
    auto result = std::make_unique<ForStatementSyntax>();
    result->forKeyword = match(SyntaxKind::ForKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind == SyntaxKind::SemicolonToken) {
        result->firstSemicolonToken = nextToken();
    } else if (isVariableDeclarationStart()) {
        result->initializer = parseVariableDeclarationStatement();
    } else {
        result->initializer = parseExpressionStatement();
    }
    if (current().kind != SyntaxKind::SemicolonToken) {
        result->condition = parseExpression();
    }
    result->secondSemicolonToken = match(SyntaxKind::SemicolonToken);
    if (current().kind != SyntaxKind::CloseParenToken) {
        result->increment = parseExpression();
    }
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->body = parseStatement();
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseForeachStatement() {
    auto result = std::make_unique<ForeachStatementSyntax>();
    result->foreachKeyword = match(SyntaxKind::ForeachKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind == SyntaxKind::RefKeyword ||
        current().kind == SyntaxKind::InKeyword) {
        result->modifierToken = nextToken();
    }
    result->type = parseType();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    while (current().kind == SyntaxKind::CommaToken) {
        result->bindingCommaTokens.push_back(nextToken());
        ForeachBindingSyntax extra;
        if (current().kind == SyntaxKind::RefKeyword ||
            current().kind == SyntaxKind::InKeyword) {
            extra.modifierToken = nextToken();
        }
        extra.type = parseType();
        extra.identifierToken = match(SyntaxKind::IdentifierToken);
        result->additionalBindings.push_back(std::move(extra));
    }
    result->inKeyword = match(SyntaxKind::InKeyword);
    result->collection = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->body = parseStatement();
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseDoWhileStatement() {
    auto result = std::make_unique<DoWhileStatementSyntax>();
    result->doKeyword = match(SyntaxKind::DoKeyword);
    result->body = parseStatement();
    result->whileKeyword = match(SyntaxKind::WhileKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->condition = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseBreakStatement() {
    auto result = std::make_unique<BreakStatementSyntax>();
    result->breakKeyword = match(SyntaxKind::BreakKeyword);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseContinueStatement() {
    auto result = std::make_unique<ContinueStatementSyntax>();
    result->continueKeyword = match(SyntaxKind::ContinueKeyword);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseSwitchStatement() {
    auto result = std::make_unique<SwitchStatementSyntax>();
    result->switchKeyword = match(SyntaxKind::SwitchKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->expression = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->openBraceToken = match(SyntaxKind::OpenBraceToken);
    bool defaultSeen = false;
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        SwitchSectionSyntax section;
        if (current().kind == SyntaxKind::CaseKeyword) {
            section.caseKeyword = nextToken();
            section.pattern = parsePattern();
            if (section.pattern && section.pattern->kind() == SyntaxKind::TypePattern) {
                const auto& tp = static_cast<const TypePatternSyntax&>(*section.pattern);
                section.patternType = tp.type;
                section.patternDesignation = tp.designationToken;
            }
            if (current().kind == SyntaxKind::WhenKeyword) {
                section.whenKeyword = nextToken();
                section.guard = parseExpression();
            }
        } else if (current().kind == SyntaxKind::DefaultKeyword) {
            section.defaultKeyword = nextToken();
            if (defaultSeen) {
                diagnostics_.report("RS1110", "switch statement has more than one default label", section.defaultKeyword->span);
            }
            defaultSeen = true;
        } else {
            diagnostics_.report("RS1109", "expected case or default label", current().span);
            nextToken();
            continue;
        }
        section.colonToken = match(SyntaxKind::ColonToken);
        parseStatementSequence(section.statements, true);
        result->sections.push_back(std::move(section));
    }
    result->closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseThrowStatement() {
    auto result = std::make_unique<ThrowStatementSyntax>();
    result->throwKeyword = nextToken();
    if (current().kind != SyntaxKind::SemicolonToken) {
        result->expression = parseExpression();
    }
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseTryStatement() {
    auto result = std::make_unique<TryStatementSyntax>();
    result->tryKeyword = nextToken();
    result->body = parseBlockStatement();
    while (current().kind == SyntaxKind::CatchKeyword) {
        CatchClauseSyntax clause;
        clause.catchKeyword = nextToken();
        if (current().kind == SyntaxKind::OpenParenToken) {
            clause.openParenToken = nextToken();
            clause.type = parseType();
            if (current().kind == SyntaxKind::IdentifierToken) {
                clause.identifierToken = nextToken();
            }
            clause.closeParenToken = match(SyntaxKind::CloseParenToken);
        }
        clause.body = parseBlockStatement();
        result->catches.push_back(std::move(clause));
    }
    if (current().kind == SyntaxKind::FinallyKeyword) {
        result->finallyKeyword = nextToken();
        result->finallyBody = parseBlockStatement();
    }
    if (result->catches.empty() && !result->finallyBody) {
        diagnostics_.report(
            "RS1120", "try statement requires catch or finally",
            result->tryKeyword.span);
    }
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseYieldStatement() {
    if (peek(1).kind == SyntaxKind::BreakKeyword) {
        auto result = std::make_unique<YieldBreakStatementSyntax>();
        result->yieldKeyword = match(SyntaxKind::YieldKeyword);
        result->breakKeyword = match(SyntaxKind::BreakKeyword);
        result->semicolonToken = match(SyntaxKind::SemicolonToken);
        return result;
    }
    auto result = std::make_unique<YieldWaitStatementSyntax>();
    result->yieldKeyword = match(SyntaxKind::YieldKeyword);
    result->waitTicksToken = match(SyntaxKind::IdentifierToken);
    if (result->waitTicksToken.text != "wait_ticks" &&
        result->waitTicksToken.text != "wait_seconds" &&
        result->waitTicksToken.text != "wait_event" &&
        result->waitTicksToken.text != "wait_signal" &&
        result->waitTicksToken.text != "wait_until" &&
        result->waitTicksToken.text != "wait_async") {
        diagnostics_.report(
            "RS1113",
            "yield currently supports only wait_ticks(expression), wait_seconds(expression), wait_event(expression), wait_until(expression), or wait_async(expression)",
            result->waitTicksToken.span);
    }
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->delay = parseExpression();
    if (current().kind == SyntaxKind::CommaToken) {
        result->commaToken = nextToken();
        result->secondArgument = parseExpression();
    }
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<ExpressionSyntax> Parser::parseEventHandler() {
    auto lambda = std::make_unique<LambdaExpressionSyntax>();
    if (current().kind == SyntaxKind::IdentifierToken &&
        peek(1).kind == SyntaxKind::ArrowToken) {
        lambda->parameterTokens.push_back(nextToken());
        lambda->arrowToken = nextToken();
        lambda->body = parseExpression();
        return lambda;
    }
    if (current().kind == SyntaxKind::OpenParenToken) {
        lambda->openParenToken = nextToken();
        if (current().kind != SyntaxKind::CloseParenToken) {
            lambda->parameterTokens.push_back(
                match(SyntaxKind::IdentifierToken));
            while (current().kind == SyntaxKind::CommaToken) {
                lambda->commaTokens.push_back(nextToken());
                lambda->parameterTokens.push_back(
                    match(SyntaxKind::IdentifierToken));
            }
        }
        lambda->closeParenToken = match(SyntaxKind::CloseParenToken);
        lambda->arrowToken = match(SyntaxKind::ArrowToken);
        lambda->body = parseExpression();
        return lambda;
    }
    return parseExpression();
}

std::unique_ptr<StatementSyntax>
Parser::parseEventSubscriptionStatement() {
    auto result =
        std::make_unique<EventSubscriptionStatementSyntax>();
    if (peek(1).kind == SyntaxKind::DotToken) {
        if (current().kind == SyntaxKind::ThisKeyword) {
            auto receiver = std::make_unique<ThisExpressionSyntax>();
            receiver->thisKeyword = nextToken();
            result->receiver = std::move(receiver);
        } else {
            auto receiver = std::make_unique<NameExpressionSyntax>();
            receiver->identifierToken = match(
                SyntaxKind::IdentifierToken);
            result->receiver = std::move(receiver);
        }
        result->dotToken = match(SyntaxKind::DotToken);
    }
    result->eventNameToken = match(SyntaxKind::IdentifierToken);
    if (current().kind == SyntaxKind::PlusEqualsToken ||
        current().kind == SyntaxKind::MinusEqualsToken) {
        result->operatorToken = nextToken();
    } else {
        result->operatorToken = match(SyntaxKind::PlusEqualsToken);
    }
    result->handler = parseEventHandler();
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

bool Parser::isLambdaExpressionStart() const noexcept {
    if (current().kind == SyntaxKind::IdentifierToken &&
        peek(1).kind == SyntaxKind::ArrowToken) {
        return true;
    }
    if (current().kind != SyntaxKind::OpenParenToken) return false;
    std::size_t offset = 1;
    bool expectParameter = true;
    while (position_ + offset < tokens_.size()) {
        const auto kind = peek(offset).kind;
        if (kind == SyntaxKind::CloseParenToken) {
            return peek(offset + 1).kind == SyntaxKind::ArrowToken &&
                (expectParameter || offset > 1);
        }
        if (expectParameter) {
            if (kind != SyntaxKind::IdentifierToken) return false;
            expectParameter = false;
        } else {
            if (kind != SyntaxKind::CommaToken) return false;
            expectParameter = true;
        }
        ++offset;
    }
    return false;
}

std::unique_ptr<StatementSyntax> Parser::parseVariableDeclarationStatement() {
    auto result = std::make_unique<VariableDeclarationStatementSyntax>();
    if (current().kind == SyntaxKind::ConstKeyword) {
        result->constKeyword = nextToken();
    }
    if (current().kind == SyntaxKind::RefKeyword) {
        result->refKeyword = nextToken();
    }
    result->type = parseType();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    if (current().kind == SyntaxKind::EqualsToken) {
        result->equalsToken = nextToken();
        if (result->refKeyword && current().kind == SyntaxKind::RefKeyword) {
            result->initializerRefKeyword = nextToken();
        }
        if (!result->refKeyword && result->type.isArray() &&
            current().kind == SyntaxKind::OpenBraceToken) {
            result->initializer = parseBraceArrayInitializer(result->type);
        } else {
            result->initializer = parseExpression();
        }
    }
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseExpressionStatement() {
    auto result = std::make_unique<ExpressionStatementSyntax>();
    result->expression = parseExpression();
    if (result->expression &&
        result->expression->kind() == SyntaxKind::MemberAccessExpression) {
        const auto& member = static_cast<const MemberAccessExpressionSyntax&>(
            *result->expression);
        if (member.nameToken.missing &&
            current().kind != SyntaxKind::SemicolonToken) {
            result->semicolonToken = SyntaxToken{
                SyntaxKind::SemicolonToken,
                {current().span.start, 0},
                "",
                {},
                true};
            return result;
        }
    }
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<ExpressionSyntax> Parser::parseExpression() {
    return parseAssignmentExpression();
}

std::unique_ptr<ExpressionSyntax> Parser::parseAssignmentExpression() {
    if (isLambdaExpressionStart()) return parseEventHandler();
    auto left = parseConditionalExpression();
    if (current().kind != SyntaxKind::EqualsToken &&
        current().kind != SyntaxKind::PlusEqualsToken &&
        current().kind != SyntaxKind::MinusEqualsToken &&
        current().kind != SyntaxKind::StarEqualsToken &&
        current().kind != SyntaxKind::SlashEqualsToken &&
        current().kind != SyntaxKind::PercentEqualsToken) {
        return left;
    }

    if (left->kind() == SyntaxKind::NameExpression) {
        auto name = std::unique_ptr<NameExpressionSyntax>(
            static_cast<NameExpressionSyntax*>(left.release()));
        auto result = std::make_unique<AssignmentExpressionSyntax>();
        result->identifierToken = std::move(name->identifierToken);
        result->equalsToken = nextToken();
        result->expression = parseAssignmentExpression();
        return result;
    }

    if (left->kind() == SyntaxKind::ElementAccessExpression) {
        auto element = std::unique_ptr<ElementAccessExpressionSyntax>(
            static_cast<ElementAccessExpressionSyntax*>(left.release()));
        auto result = std::make_unique<ElementAssignmentExpressionSyntax>();
        result->receiver = std::move(element->receiver);
        result->openBracketToken = std::move(element->openBracketToken);
        result->index = std::move(element->index);
        result->closeBracketToken = std::move(element->closeBracketToken);
        result->equalsToken = nextToken();
        result->expression = parseAssignmentExpression();
        return result;
    }

    if (left->kind() == SyntaxKind::MemberAccessExpression) {
        auto member = std::unique_ptr<MemberAccessExpressionSyntax>(
            static_cast<MemberAccessExpressionSyntax*>(left.release()));
        auto result = std::make_unique<MemberAssignmentExpressionSyntax>();
        result->receiver = std::move(member->receiver);
        result->dotToken = std::move(member->dotToken);
        result->nameToken = std::move(member->nameToken);
        result->equalsToken = nextToken();
        result->expression = parseAssignmentExpression();
        return result;
    }

    return left;
}

std::unique_ptr<ExpressionSyntax> Parser::parseConditionalExpression() {
    auto condition = parseNullCoalescingExpression();
    if (current().kind == SyntaxKind::SwitchKeyword) {
        auto switchExpression = std::make_unique<SwitchExpressionSyntax>();
        switchExpression->expression = std::move(condition);
        switchExpression->switchKeyword = nextToken();
        switchExpression->openBraceToken = match(SyntaxKind::OpenBraceToken);
        while (current().kind != SyntaxKind::CloseBraceToken &&
               current().kind != SyntaxKind::EndOfFileToken) {
            SwitchExpressionArmSyntax arm;
            arm.pattern = parsePattern();
            if (arm.pattern) {
                if (arm.pattern->kind() == SyntaxKind::DiscardPattern) {
                    arm.discardToken = static_cast<const DiscardPatternSyntax&>(*arm.pattern).underscoreToken;
                } else if (arm.pattern->kind() == SyntaxKind::TypePattern) {
                    const auto& tp = static_cast<const TypePatternSyntax&>(*arm.pattern);
                    arm.patternType = tp.type;
                    arm.patternDesignation = tp.designationToken;
                }
            }
            if (current().kind == SyntaxKind::WhenKeyword) {
                arm.whenKeyword = nextToken();
                arm.guard = parseExpression();
            }
            arm.arrowToken = match(SyntaxKind::ArrowToken);
            arm.value = parseExpression();
            switchExpression->arms.push_back(std::move(arm));
            if (current().kind != SyntaxKind::CommaToken) break;
            switchExpression->commaTokens.push_back(nextToken());
            if (current().kind == SyntaxKind::CloseBraceToken) break;
        }
        switchExpression->closeBraceToken = match(SyntaxKind::CloseBraceToken);
        condition = std::move(switchExpression);
    }
    if (current().kind != SyntaxKind::QuestionToken) return condition;

    auto result = std::make_unique<ConditionalExpressionSyntax>();
    result->condition = std::move(condition);
    result->questionToken = nextToken();
    result->whenTrue = parseAssignmentExpression();
    result->colonToken = match(SyntaxKind::ColonToken);
    result->whenFalse = parseAssignmentExpression();
    return result;
}

std::unique_ptr<ExpressionSyntax> Parser::parseNullCoalescingExpression() {
    auto left = parseBinaryExpression();
    if (current().kind != SyntaxKind::QuestionQuestionToken) return left;
    auto result = std::make_unique<BinaryExpressionSyntax>();
    result->left = std::move(left);
    result->operatorToken = nextToken();
    result->right = parseNullCoalescingExpression();
    return result;
}

std::unique_ptr<ExpressionSyntax> Parser::parseBinaryExpression(int parentPrecedence) {
    std::unique_ptr<ExpressionSyntax> left;
    const auto unary = unaryPrecedence(current().kind);
    if (unary != 0 && unary >= parentPrecedence) {
        auto expression = std::make_unique<UnaryExpressionSyntax>();
        expression->operatorToken = nextToken();
        expression->operand = parseBinaryExpression(unary);
        left = std::move(expression);
    } else {
        left = parsePostfixExpression();
    }

    for (;;) {
        const auto typeOperator = current().kind == SyntaxKind::IsKeyword ||
            current().kind == SyntaxKind::AsKeyword;
        const auto precedence = typeOperator
            ? 4
            : binaryPrecedence(current().kind);
        if (precedence == 0 || precedence <= parentPrecedence) {
            break;
        }
        if (typeOperator) {
            const auto operatorToken = nextToken();
            auto expression = std::make_unique<TypeBinaryExpressionSyntax>();
            expression->expression = std::move(left);
            expression->operatorToken = operatorToken;
            if (operatorToken.kind == SyntaxKind::IsKeyword) {
                expression->pattern = parsePattern();
                if (expression->pattern &&
                    expression->pattern->kind() == SyntaxKind::TypePattern) {
                    const auto& tp = static_cast<const TypePatternSyntax&>(*expression->pattern);
                    expression->type = tp.type;
                    expression->designationToken = tp.designationToken;
                }
            } else {
                expression->type = parseType();
            }
            left = std::move(expression);
            continue;
        }
        auto expression = std::make_unique<BinaryExpressionSyntax>();
        expression->left = std::move(left);
        expression->operatorToken = nextToken();
        expression->right = parseBinaryExpression(precedence);
        left = std::move(expression);
    }
    return left;
}

std::unique_ptr<ExpressionSyntax> Parser::parsePostfixExpression() {
    auto expression = parsePrimaryExpression();
    for (;;) {
        if (current().kind == SyntaxKind::DotToken ||
            current().kind == SyntaxKind::QuestionDotToken) {
            const auto dot = nextToken();
            const bool genericCall = isGenericCallStart();
            const auto name = match(SyntaxKind::IdentifierToken);
            std::optional<SyntaxToken> lessToken;
            std::vector<TypeSyntax> typeArguments;
            std::vector<SyntaxToken> typeArgumentCommas;
            std::optional<SyntaxToken> greaterToken;
            if (genericCall) {
                parseTypeArgumentList(
                    lessToken, typeArguments, typeArgumentCommas,
                    greaterToken);
            }
            if (current().kind == SyntaxKind::OpenParenToken) {
                auto call = std::make_unique<MemberCallExpressionSyntax>();
                call->receiver = std::move(expression);
                call->dotToken = dot;
                call->nameToken = name;
                call->lessToken = std::move(lessToken);
                call->typeArguments = std::move(typeArguments);
                call->typeArgumentCommaTokens =
                    std::move(typeArgumentCommas);
                call->greaterToken = std::move(greaterToken);
                call->openParenToken = nextToken();
                parseArgumentList(
                    call->arguments,
                    &call->argumentModifiers,
                    call->commaTokens,
                    call->closeParenToken,
                    &call->argumentNames,
                    &call->argumentNameColonTokens);
                expression = std::move(call);
            } else {
                if (lessToken) {
                    diagnostics_.report(
                        "RS1115",
                        "generic member type arguments require a call",
                        lessToken->span);
                }
                auto member = std::make_unique<MemberAccessExpressionSyntax>();
                member->receiver = std::move(expression);
                member->dotToken = dot;
                member->nameToken = name;
                expression = std::move(member);
            }
            continue;
        }
        if (current().kind == SyntaxKind::OpenBracketToken) {
            auto element = std::make_unique<ElementAccessExpressionSyntax>();
            element->receiver = std::move(expression);
            element->openBracketToken = nextToken();
            element->index = parseExpression();
            element->closeBracketToken = match(SyntaxKind::CloseBracketToken);
            expression = std::move(element);
            continue;
        }
        break;
    }
    return expression;
}

std::unique_ptr<ExpressionSyntax> Parser::parseNewExpression() {
    const auto newKeyword = match(SyntaxKind::NewKeyword);
    auto type = parseType();

    if (!type.isArray() &&
        (current().kind == SyntaxKind::OpenParenToken ||
         current().kind == SyntaxKind::OpenBraceToken)) {
        auto result = std::make_unique<NewObjectExpressionSyntax>();
        result->newKeyword = newKeyword;
        result->type = std::move(type);
        if (current().kind == SyntaxKind::OpenParenToken) {
            result->openParenToken = nextToken();
            parseArgumentList(
                result->arguments, nullptr, result->commaTokens,
                result->closeParenToken, &result->argumentNames,
                &result->argumentNameColonTokens);
        } else {
            result->openParenToken.kind = SyntaxKind::OpenParenToken;
            result->openParenToken.span = result->type.span();
            result->openParenToken.text = "(";
            result->openParenToken.missing = true;
            result->closeParenToken.kind = SyntaxKind::CloseParenToken;
            result->closeParenToken.span = result->type.span();
            result->closeParenToken.text = ")";
            result->closeParenToken.missing = true;
        }
        if (current().kind == SyntaxKind::OpenBraceToken) {
            result->initializerOpenBraceToken = nextToken();
            while (current().kind != SyntaxKind::CloseBraceToken &&
                   current().kind != SyntaxKind::EndOfFileToken) {
                InitializerElementSyntax element;
                if (current().kind == SyntaxKind::IdentifierToken &&
                    peek(1).kind == SyntaxKind::EqualsToken) {
                    element.nameToken = nextToken();
                    element.equalsToken = nextToken();
                    element.expressions.push_back(parseExpression());
                } else if (current().kind == SyntaxKind::OpenBraceToken) {
                    element.openBraceToken = nextToken();
                    if (current().kind != SyntaxKind::CloseBraceToken) {
                        element.expressions.push_back(parseExpression());
                        while (current().kind == SyntaxKind::CommaToken) {
                            element.commaTokens.push_back(nextToken());
                            if (current().kind == SyntaxKind::CloseBraceToken) break;
                            element.expressions.push_back(parseExpression());
                        }
                    }
                    element.closeBraceToken =
                        match(SyntaxKind::CloseBraceToken);
                } else {
                    element.expressions.push_back(parseExpression());
                }
                result->initializers.push_back(std::move(element));
                if (current().kind != SyntaxKind::CommaToken) break;
                result->initializerCommaTokens.push_back(nextToken());
                if (current().kind == SyntaxKind::CloseBraceToken) break;
            }
            result->initializerCloseBraceToken =
                match(SyntaxKind::CloseBraceToken);
        }
        return result;
    }

    auto result = std::make_unique<NewArrayExpressionSyntax>();
    result->newKeyword = newKeyword;
    if (type.isArray()) {
        result->openBracketToken = *type.openBracketToken;
        result->closeBracketToken = *type.closeBracketToken;
        type.openBracketToken.reset();
        type.closeBracketToken.reset();
        result->elementType = std::move(type);
    } else {
        result->elementType = std::move(type);
        result->openBracketToken = match(SyntaxKind::OpenBracketToken);
        if (current().kind != SyntaxKind::CloseBracketToken) {
            result->length = parseExpression();
            result->explicitLength = true;
        }
        result->closeBracketToken = match(SyntaxKind::CloseBracketToken);
    }
    if (current().kind == SyntaxKind::OpenBraceToken) {
        result->initializerOpenBraceToken = nextToken();
        while (current().kind != SyntaxKind::CloseBraceToken &&
               current().kind != SyntaxKind::EndOfFileToken) {
            result->initialValues.push_back(parseExpression());
            if (current().kind != SyntaxKind::CommaToken) break;
            result->initializerCommaTokens.push_back(nextToken());
            if (current().kind == SyntaxKind::CloseBraceToken) break;
        }
        result->initializerCloseBraceToken = match(SyntaxKind::CloseBraceToken);
    }
    if (!result->length) {
        if (!result->initializerOpenBraceToken) {
            diagnostics_.report(
                "RS1100",
                "array creation must specify an array size or array initializer",
                result->closeBracketToken.span);
        }
        auto lenLit = std::make_unique<LiteralExpressionSyntax>();
        const auto countVal = static_cast<std::int64_t>(result->initialValues.size());
        lenLit->literalToken = SyntaxToken{
            SyntaxKind::IntegerLiteralToken,
            result->closeBracketToken.span,
            std::to_string(countVal),
            countVal,
            true};
        result->length = std::move(lenLit);
    }
    return result;
}

std::unique_ptr<NewArrayExpressionSyntax> Parser::parseBraceArrayInitializer(
    const TypeSyntax& arrayType) {
    auto arr = std::make_unique<NewArrayExpressionSyntax>();
    arr->newKeyword = SyntaxToken{
        SyntaxKind::NewKeyword, {current().span.start, 0}, "new", {}, true};
    arr->elementType = arrayType;
    arr->elementType.openBracketToken.reset();
    arr->elementType.closeBracketToken.reset();
    arr->openBracketToken = *arrayType.openBracketToken;
    arr->closeBracketToken = *arrayType.closeBracketToken;
    arr->initializerOpenBraceToken = nextToken();
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        arr->initialValues.push_back(parseExpression());
        if (current().kind != SyntaxKind::CommaToken) break;
        arr->initializerCommaTokens.push_back(nextToken());
        if (current().kind == SyntaxKind::CloseBraceToken) break;
    }
    arr->initializerCloseBraceToken = match(SyntaxKind::CloseBraceToken);
    auto lenLit = std::make_unique<LiteralExpressionSyntax>();
    const auto countVal = static_cast<std::int64_t>(arr->initialValues.size());
    lenLit->literalToken = SyntaxToken{
        SyntaxKind::IntegerLiteralToken,
        arr->initializerOpenBraceToken->span,
        std::to_string(countVal),
        countVal,
        true};
    arr->length = std::move(lenLit);
    arr->explicitLength = false;
    return arr;
}

std::unique_ptr<ExpressionSyntax> Parser::parsePrimaryExpression() {
    switch (current().kind) {
    case SyntaxKind::TypeofKeyword: {
        auto result = std::make_unique<TypeOfExpressionSyntax>();
        result->typeofKeyword = nextToken();
        result->openParenToken = match(SyntaxKind::OpenParenToken);
        result->type = parseType();
        result->closeParenToken = match(SyntaxKind::CloseParenToken);
        return result;
    }
    case SyntaxKind::NewKeyword:
        return parseNewExpression();
    case SyntaxKind::ThisKeyword: {
        auto result = std::make_unique<ThisExpressionSyntax>();
        result->thisKeyword = nextToken();
        return result;
    }
    case SyntaxKind::BaseKeyword: {
        auto result = std::make_unique<BaseExpressionSyntax>();
        result->baseKeyword = nextToken();
        return result;
    }
    case SyntaxKind::OpenParenToken: {
        const auto startsOperandAfterCast = [](SyntaxKind k) noexcept {
            switch (k) {
            case SyntaxKind::IdentifierToken:
            case SyntaxKind::IntegerLiteralToken:
            case SyntaxKind::FloatLiteralToken:
            case SyntaxKind::StringLiteralToken:
            case SyntaxKind::CharLiteralToken:
            case SyntaxKind::MinusToken:
            case SyntaxKind::PlusToken:
            case SyntaxKind::InterpolatedStringToken:
            case SyntaxKind::TrueKeyword:
            case SyntaxKind::FalseKeyword:
            case SyntaxKind::NullKeyword:
            case SyntaxKind::ThisKeyword:
            case SyntaxKind::BaseKeyword:
            case SyntaxKind::NewKeyword:
            case SyntaxKind::TypeofKeyword:
            case SyntaxKind::OpenParenToken:
            case SyntaxKind::BangToken:
                return true;
            default:
                return false;
            }
        };
        if ((isPrimitiveTypeKeyword(peek(1).kind) &&
             peek(2).kind == SyntaxKind::CloseParenToken) ||
            (peek(1).kind == SyntaxKind::IdentifierToken &&
             peek(2).kind == SyntaxKind::CloseParenToken &&
             startsOperandAfterCast(peek(3).kind))) {
            auto result = std::make_unique<CastExpressionSyntax>();
            result->openParenToken = nextToken();
            result->type = parseType();
            result->closeParenToken = match(SyntaxKind::CloseParenToken);
            result->expression = parseBinaryExpression(7);
            return result;
        }
        auto result = std::make_unique<ParenthesizedExpressionSyntax>();
        result->openParenToken = nextToken();
        result->expression = parseExpression();
        result->closeParenToken = match(SyntaxKind::CloseParenToken);
        return result;
    }
    case SyntaxKind::TrueKeyword:
    case SyntaxKind::FalseKeyword:
    case SyntaxKind::NullKeyword:
    case SyntaxKind::IntegerLiteralToken:
    case SyntaxKind::FloatLiteralToken:
    case SyntaxKind::StringLiteralToken:
    case SyntaxKind::CharLiteralToken: {
        auto result = std::make_unique<LiteralExpressionSyntax>();
        result->literalToken = nextToken();
        return result;
    }
    case SyntaxKind::InterpolatedStringToken: {
        const auto token = nextToken();
        const std::string_view raw(token.text);
        const std::size_t end = (raw.size() >= 3 && raw.back() == '"')
            ? raw.size() - 1
            : raw.size();
        auto makeStringLiteral = [&](std::string val, text::TextSpan span)
            -> std::unique_ptr<ExpressionSyntax> {
            auto lit = std::make_unique<LiteralExpressionSyntax>();
            lit->literalToken.kind = SyntaxKind::StringLiteralToken;
            lit->literalToken.span = span;
            lit->literalToken.text = "\"" + val + "\"";
            lit->literalToken.value = std::move(val);
            return lit;
        };
        auto appendPart = [&](std::unique_ptr<ExpressionSyntax> accum,
                              std::unique_ptr<ExpressionSyntax> part,
                              text::TextSpan opSpan)
            -> std::unique_ptr<ExpressionSyntax> {
            if (!accum) return part;
            auto bin = std::make_unique<BinaryExpressionSyntax>();
            bin->left = std::move(accum);
            bin->operatorToken.kind = SyntaxKind::PlusToken;
            bin->operatorToken.span = opSpan;
            bin->operatorToken.text = "+";
            bin->right = std::move(part);
            return bin;
        };

        std::unique_ptr<ExpressionSyntax> root;
        std::string currentText;
        std::size_t segmentStart = token.span.start;
        std::size_t i = raw.size() >= 2 ? 2 : raw.size();
        bool hasHole = false;

        while (i < end) {
            if (raw[i] == '\\' && i + 1 < end) {
                const char esc = raw[i + 1];
                switch (esc) {
                case 'n': currentText.push_back('\n'); break;
                case 'r': currentText.push_back('\r'); break;
                case 't': currentText.push_back('\t'); break;
                case '\\': currentText.push_back('\\'); break;
                case '"': currentText.push_back('"'); break;
                case '{': currentText.push_back('{'); break;
                case '}': currentText.push_back('}'); break;
                default: currentText.push_back(esc); break;
                }
                i += 2;
                continue;
            }
            if (raw[i] == '}' && i + 1 < end && raw[i + 1] == '}') {
                currentText.push_back('}');
                i += 2;
                continue;
            }
            if (raw[i] == '{') {
                if (i + 1 < end && raw[i + 1] == '{') {
                    currentText.push_back('{');
                    i += 2;
                    continue;
                }
                hasHole = true;
                const text::TextSpan textSpan{
                    segmentStart,
                    (token.span.start + i) - segmentStart};
                if (!currentText.empty() || !root) {
                    root = appendPart(
                        std::move(root),
                        makeStringLiteral(std::move(currentText), textSpan),
                        textSpan);
                    currentText.clear();
                }

                const std::size_t exprStart = i + 1;
                std::size_t j = exprStart;
                enum class HoleModeKind { InterpText, InterpHole, NormalString };
                struct HoleMode {
                    HoleModeKind kind;
                    int braceDepth = 0;
                };
                std::vector<HoleMode> modeStack{{HoleModeKind::InterpHole, 1}};
                while (j < end && !modeStack.empty()) {
                    const auto mode = modeStack.back().kind;
                    if (mode == HoleModeKind::NormalString) {
                        if (raw[j] == '\\' && j + 1 < end) {
                            j += 2;
                            continue;
                        }
                        if (raw[j] == '"') {
                            modeStack.pop_back();
                        }
                        ++j;
                    } else if (mode == HoleModeKind::InterpText) {
                        if (raw[j] == '\\' && j + 1 < end) {
                            j += 2;
                            continue;
                        }
                        if (raw[j] == '"') {
                            modeStack.pop_back();
                            ++j;
                            continue;
                        }
                        if (raw[j] == '{') {
                            if (j + 1 < end && raw[j + 1] == '{') {
                                j += 2;
                                continue;
                            }
                            modeStack.push_back({HoleModeKind::InterpHole, 1});
                            ++j;
                            continue;
                        }
                        if (raw[j] == '}' && j + 1 < end && raw[j + 1] == '}') {
                            j += 2;
                            continue;
                        }
                        ++j;
                    } else {
                        if (raw[j] == '$' && j + 1 < end && raw[j + 1] == '"') {
                            modeStack.push_back({HoleModeKind::InterpText, 0});
                            j += 2;
                            continue;
                        }
                        if (raw[j] == '"') {
                            modeStack.push_back({HoleModeKind::NormalString, 0});
                            ++j;
                            continue;
                        }
                        if (raw[j] == '\'') {
                            ++j;
                            if (j < end && raw[j] == '\\' && j + 1 < end) {
                                j += 2;
                            } else if (j < end) {
                                ++j;
                            }
                            if (j < end && raw[j] == '\'') {
                                ++j;
                            }
                            continue;
                        }
                        if (raw[j] == '{') {
                            ++modeStack.back().braceDepth;
                            ++j;
                            continue;
                        }
                        if (raw[j] == '}') {
                            --modeStack.back().braceDepth;
                            if (modeStack.back().braceDepth == 0) {
                                modeStack.pop_back();
                                if (modeStack.empty()) {
                                    break;
                                }
                            }
                            ++j;
                            continue;
                        }
                        ++j;
                    }
                }
                const auto holeSpan = text::TextSpan{
                    token.span.start + i,
                    (j < end ? j + 1 : end) - i};
                if (!modeStack.empty()) {
                    diagnostics_.report(
                        "RS1001",
                        "unterminated interpolation hole in string literal",
                        holeSpan);
                }
                const auto exprSlice = raw.substr(exprStart, j - exprStart);
                std::string padded(token.span.start + exprStart, ' ');
                padded.append(exprSlice);
                text::SourceText subSource(std::move(padded), source_.name());
                Parser subParser(subSource, diagnostics_);
                std::unique_ptr<ExpressionSyntax> innerExpr;
                if (subParser.current().kind == SyntaxKind::EndOfFileToken) {
                    diagnostics_.report(
                        "RS1121",
                        "interpolated string hole cannot be empty",
                        holeSpan);
                    innerExpr = makeStringLiteral("", holeSpan);
                } else {
                    innerExpr = subParser.parseExpression();
                }
                auto paren = std::make_unique<ParenthesizedExpressionSyntax>();
                paren->openParenToken = {
                    SyntaxKind::OpenParenToken,
                    {token.span.start + i, 1}, "(", {}, true};
                paren->expression = std::move(innerExpr);
                paren->closeParenToken = {
                    SyntaxKind::CloseParenToken,
                    {token.span.start + (j < end ? j : end - 1), 1},
                    ")", {}, true};
                root = appendPart(std::move(root), std::move(paren), holeSpan);
                i = (j < end) ? j + 1 : end;
                segmentStart = token.span.start + i;
                continue;
            }
            currentText.push_back(raw[i]);
            ++i;
        }

        if (!hasHole) {
            return makeStringLiteral(std::move(currentText), token.span);
        }
        if (!currentText.empty()) {
            const text::TextSpan tailSpan{
                segmentStart,
                token.span.end() - segmentStart};
            root = appendPart(
                std::move(root),
                makeStringLiteral(std::move(currentText), tailSpan),
                tailSpan);
        }
        return root ? std::move(root) : makeStringLiteral("", token.span);
    }
    case SyntaxKind::IdentifierToken: {
        const auto qualLen = moduleQualifiedIdentLength(0);
        const std::size_t nextOff = qualLen > 0 ? qualLen : 1;
        if (peek(nextOff).kind == SyntaxKind::OpenParenToken ||
            isGenericCallStart()) {
            return parseCallExpression();
        } else if (isGenericTypeMemberAccessStart()) {
            auto result = std::make_unique<NameExpressionSyntax>();
            result->identifierToken = qualLen > 0
                ? consumeModuleQualifiedIdentifier()
                : nextToken();
            parseTypeArgumentList(
                result->lessToken,
                result->typeArguments,
                result->typeArgumentCommaTokens,
                result->greaterToken);
            return result;
        } else {
            auto result = std::make_unique<NameExpressionSyntax>();
            result->identifierToken = qualLen > 0
                ? consumeModuleQualifiedIdentifier()
                : nextToken();
            return result;
        }
    }
    default: {
        auto result = std::make_unique<NameExpressionSyntax>();
        result->identifierToken = match(SyntaxKind::IdentifierToken);
        return result;
    }
    }
}

std::size_t Parser::moduleQualifiedIdentLength(
    std::size_t baseOffset) const noexcept {
    if (!isIdentifierLike(peek(baseOffset).kind)) {
        return 0;
    }
    std::size_t cur = baseOffset + 1;
    while (peek(cur).kind == SyntaxKind::DotToken &&
           isIdentifierLike(peek(cur + 1).kind)) {
        cur += 2;
    }
    if (peek(cur).kind == SyntaxKind::ColonToken &&
        peek(cur + 1).kind == SyntaxKind::ColonToken &&
        isIdentifierLike(peek(cur + 2).kind)) {
        return (cur + 3) - baseOffset;
    }
    return 0;
}

SyntaxToken Parser::consumeModuleQualifiedIdentifier() {
    const auto len = moduleQualifiedIdentLength(0);
    if (len == 0) {
        if (isIdentifierLike(current().kind)) {
            return nextToken();
        }
        return match(SyntaxKind::IdentifierToken);
    }
    const auto firstTok = nextToken();
    std::string combined = firstTok.text;
    text::TextSpan lastSpan = firstTok.span;
    for (std::size_t i = 1; i < len; ++i) {
        const auto tok = nextToken();
        combined += tok.text;
        lastSpan = tok.span;
    }
    const std::size_t totalLen =
        (lastSpan.start + lastSpan.length >= firstTok.span.start)
            ? (lastSpan.start + lastSpan.length - firstTok.span.start)
            : firstTok.span.length;
    return SyntaxToken{
        SyntaxKind::IdentifierToken,
        {firstTok.span.start, totalLen},
        std::move(combined),
        {},
        false};
}

bool Parser::isGenericTypeMemberAccessStart() const noexcept {
    const auto qualLen = moduleQualifiedIdentLength(0);
    const std::size_t lessOff = qualLen > 0 ? qualLen : 1;
    if (current().kind != SyntaxKind::IdentifierToken ||
        peek(lessOff).kind != SyntaxKind::LessToken) {
        return false;
    }
    int depth = 0;
    std::size_t offset = lessOff;
    while (position_ + offset < tokens_.size()) {
        const auto kind = peek(offset).kind;
        if (kind == SyntaxKind::LessToken) {
            ++depth;
        } else if (kind == SyntaxKind::GreaterToken) {
            --depth;
            if (depth == 0) {
                return peek(offset + 1).kind == SyntaxKind::DotToken;
            }
        } else if (isIdentifierLike(kind) ||
                   kind == SyntaxKind::DotToken ||
                   kind == SyntaxKind::ColonToken ||
                   kind == SyntaxKind::CommaToken ||
                   kind == SyntaxKind::QuestionToken ||
                   kind == SyntaxKind::OpenBracketToken ||
                   kind == SyntaxKind::CloseBracketToken) {
        } else {
            return false;
        }
        ++offset;
    }
    return false;
}

bool Parser::isGenericCallStart() const noexcept {
    const auto qualLen = moduleQualifiedIdentLength(0);
    const std::size_t lessOff = qualLen > 0 ? qualLen : 1;
    if (current().kind != SyntaxKind::IdentifierToken ||
        peek(lessOff).kind != SyntaxKind::LessToken) {
        return false;
    }
    int depth = 0;
    std::size_t offset = lessOff;
    while (position_ + offset < tokens_.size()) {
        const auto kind = peek(offset).kind;
        if (kind == SyntaxKind::LessToken) {
            ++depth;
        } else if (kind == SyntaxKind::GreaterToken) {
            --depth;
            if (depth == 0) {
                return peek(offset + 1).kind ==
                    SyntaxKind::OpenParenToken;
            }
        } else if (isIdentifierLike(kind) ||
                   kind == SyntaxKind::DotToken ||
                   kind == SyntaxKind::ColonToken ||
                   kind == SyntaxKind::CommaToken ||
                   kind == SyntaxKind::QuestionToken ||
                   kind == SyntaxKind::OpenBracketToken ||
                   kind == SyntaxKind::CloseBracketToken) {
            // valid token inside a generic type argument list
        } else {
            return false;
        }
        ++offset;
    }
    return false;
}

std::unique_ptr<ExpressionSyntax> Parser::parseCallExpression() {
    auto result = std::make_unique<CallExpressionSyntax>();
    result->identifierToken = moduleQualifiedIdentLength(0) > 0
        ? consumeModuleQualifiedIdentifier()
        : match(SyntaxKind::IdentifierToken);
    parseTypeArgumentList(
        result->lessToken,
        result->typeArguments,
        result->typeArgumentCommaTokens,
        result->greaterToken);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    parseArgumentList(
        result->arguments, &result->argumentModifiers,
        result->commaTokens, result->closeParenToken,
        &result->argumentNames,
        &result->argumentNameColonTokens);
    return result;
}

bool Parser::isVariableDeclarationStartAt(
    std::size_t baseOffset,
    bool requireInitializer) const noexcept {
    std::size_t start = baseOffset;
    if (peek(start).kind == SyntaxKind::ConstKeyword) ++start;
    if (peek(start).kind == SyntaxKind::RefKeyword) ++start;
    if (!isIdentifierLike(peek(start).kind)) {
        return false;
    }
    const auto qualLen = moduleQualifiedIdentLength(start);
    std::size_t offset = qualLen > 0 ? (start + qualLen) : (start + 1);
    if (qualLen == 0) {
        while (peek(offset).kind == SyntaxKind::DotToken &&
               isIdentifierLike(peek(offset + 1).kind)) {
            offset += 2;
        }
    }
    if (peek(offset).kind == SyntaxKind::LessToken) {
        int depth = 0;
        do {
            if (peek(offset).kind == SyntaxKind::LessToken) ++depth;
            else if (peek(offset).kind == SyntaxKind::GreaterToken) --depth;
            ++offset;
            if (peek(offset).kind == SyntaxKind::EndOfFileToken) {
                return false;
            }
        } while (depth > 0);
    }
    if (peek(offset).kind == SyntaxKind::QuestionToken) ++offset;
    while (peek(offset).kind == SyntaxKind::OpenBracketToken &&
           peek(offset + 1).kind == SyntaxKind::CloseBracketToken) {
        offset += 2;
    }
    if (peek(offset).kind != SyntaxKind::IdentifierToken) {
        return false;
    }
    if (requireInitializer) {
        return peek(offset + 1).kind == SyntaxKind::EqualsToken;
    }
    return peek(offset + 1).kind == SyntaxKind::EqualsToken ||
        peek(offset + 1).kind == SyntaxKind::SemicolonToken;
}

bool Parser::isVariableDeclarationStart() const noexcept {
    return isVariableDeclarationStartAt(0, false);
}

bool Parser::isUsingDeclarationStart() const noexcept {
    return current().kind == SyntaxKind::IdentifierToken &&
        current().text == "using" &&
        isVariableDeclarationStartAt(1, false);
}


namespace {

bool isPatternDesignation(const SyntaxToken& token) noexcept {
    return token.kind == SyntaxKind::IdentifierToken &&
        token.text != "and" &&
        token.text != "or" &&
        token.text != "not";
}

bool isArithmeticContinuation(SyntaxKind kind) noexcept {
    return kind == SyntaxKind::PlusToken ||
        kind == SyntaxKind::MinusToken ||
        kind == SyntaxKind::StarToken ||
        kind == SyntaxKind::SlashToken ||
        kind == SyntaxKind::PercentToken;
}

} // namespace

bool Parser::canStartPattern(std::size_t offset) const noexcept {
    const auto& tok = peek(offset);
    switch (tok.kind) {
    case SyntaxKind::LessToken:
    case SyntaxKind::LessOrEqualsToken:
    case SyntaxKind::GreaterToken:
    case SyntaxKind::GreaterOrEqualsToken:
    case SyntaxKind::OpenBraceToken:
    case SyntaxKind::OpenParenToken:
    case SyntaxKind::NullKeyword:
    case SyntaxKind::TrueKeyword:
    case SyntaxKind::FalseKeyword:
    case SyntaxKind::IntegerLiteralToken:
    case SyntaxKind::FloatLiteralToken:
    case SyntaxKind::StringLiteralToken:
    case SyntaxKind::CharLiteralToken:
    case SyntaxKind::MinusToken:
    case SyntaxKind::PlusToken:
    case SyntaxKind::IdentifierToken:
        return true;
    default:
        return isPrimitiveTypeKeyword(tok.kind);
    }
}

std::unique_ptr<PatternSyntax> Parser::parsePattern() {
    return parseOrPattern();
}

std::unique_ptr<PatternSyntax> Parser::parseOrPattern() {
    auto left = parseAndPattern();
    while (current().kind == SyntaxKind::IdentifierToken &&
           current().text == "or" &&
           canStartPattern(1)) {
        auto bin = std::make_unique<BinaryPatternSyntax>();
        bin->left = std::move(left);
        bin->operatorToken = nextToken();
        bin->right = parseAndPattern();
        left = std::move(bin);
    }
    return left;
}

std::unique_ptr<PatternSyntax> Parser::parseAndPattern() {
    auto left = parseUnaryPattern();
    while (current().kind == SyntaxKind::IdentifierToken &&
           current().text == "and" &&
           canStartPattern(1)) {
        auto bin = std::make_unique<BinaryPatternSyntax>();
        bin->left = std::move(left);
        bin->operatorToken = nextToken();
        bin->right = parseUnaryPattern();
        left = std::move(bin);
    }
    return left;
}

std::unique_ptr<PatternSyntax> Parser::parseUnaryPattern() {
    if (current().kind == SyntaxKind::IdentifierToken &&
        current().text == "not" &&
        canStartPattern(1)) {
        auto unary = std::make_unique<UnaryPatternSyntax>();
        unary->operatorToken = nextToken();
        unary->pattern = parseUnaryPattern();
        return unary;
    }
    return parsePrimaryPattern();
}

PropertyPatternSyntax Parser::parsePropertyPatternTail(std::optional<TypeSyntax> leadingType) {
    PropertyPatternSyntax prop;
    prop.type = std::move(leadingType);
    prop.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        PropertySubpatternSyntax sub;
        sub.memberToken = match(SyntaxKind::IdentifierToken);
        sub.colonToken = match(SyntaxKind::ColonToken);
        sub.pattern = parsePattern();
        prop.properties.push_back(std::move(sub));
        if (current().kind != SyntaxKind::CommaToken) break;
        prop.commaTokens.push_back(nextToken());
        if (current().kind == SyntaxKind::CloseBraceToken) break;
    }
    prop.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    if (isPatternDesignation(current())) {
        prop.designationToken = nextToken();
    }
    return prop;
}

std::unique_ptr<PatternSyntax> Parser::parsePrimaryPattern() {
    // 1. Relational pattern: <, <=, >, >= followed by constant expression (precedence > 4)
    if (current().kind == SyntaxKind::LessToken ||
        current().kind == SyntaxKind::LessOrEqualsToken ||
        current().kind == SyntaxKind::GreaterToken ||
        current().kind == SyntaxKind::GreaterOrEqualsToken) {
        auto rel = std::make_unique<RelationalPatternSyntax>();
        rel->operatorToken = nextToken();
        rel->operand = parseBinaryExpression(4);
        return rel;
    }

    // 2. Anonymous property pattern: { Prop: subpattern, ... } [designation]
    if (current().kind == SyntaxKind::OpenBraceToken) {
        return std::make_unique<PropertyPatternSyntax>(parsePropertyPatternTail(std::nullopt));
    }

    // 3. Parenthesized pattern vs parenthesized / cast constant expression
    if (current().kind == SyntaxKind::OpenParenToken) {
        const auto startsAfterCast = [](SyntaxKind k) noexcept {
            switch (k) {
            case SyntaxKind::IdentifierToken:
            case SyntaxKind::IntegerLiteralToken:
            case SyntaxKind::FloatLiteralToken:
            case SyntaxKind::StringLiteralToken:
            case SyntaxKind::CharLiteralToken:
            case SyntaxKind::InterpolatedStringToken:
            case SyntaxKind::TrueKeyword:
            case SyntaxKind::FalseKeyword:
            case SyntaxKind::NullKeyword:
            case SyntaxKind::OpenParenToken:
            case SyntaxKind::MinusToken:
            case SyntaxKind::PlusToken:
            case SyntaxKind::BangToken:
                return true;
            default:
                return false;
            }
        };
        const bool isCastConst =
            (isPrimitiveTypeKeyword(peek(1).kind) &&
             peek(2).kind == SyntaxKind::CloseParenToken &&
             startsAfterCast(peek(3).kind)) ||
            (peek(1).kind == SyntaxKind::IdentifierToken &&
             peek(2).kind == SyntaxKind::CloseParenToken &&
             startsAfterCast(peek(3).kind));
        if (isCastConst) {
            auto cp = std::make_unique<ConstantPatternSyntax>();
            cp->expression = parseBinaryExpression(4);
            return cp;
        }

        // Scan balanced parens to distinguish arithmetic constant expression (1 + 2) from pattern
        std::size_t depth = 1;
        std::size_t offset = 1;
        bool hasTopLevelArithmetic = false;
        while (peek(offset).kind != SyntaxKind::EndOfFileToken && depth > 0) {
            const auto& tk = peek(offset);
            if (tk.kind == SyntaxKind::OpenParenToken ||
                tk.kind == SyntaxKind::OpenBraceToken ||
                tk.kind == SyntaxKind::OpenBracketToken) {
                ++depth;
            } else if (tk.kind == SyntaxKind::CloseParenToken ||
                       tk.kind == SyntaxKind::CloseBraceToken ||
                       tk.kind == SyntaxKind::CloseBracketToken) {
                --depth;
                if (depth == 0) break;
            } else if (depth == 1) {
                if (tk.kind == SyntaxKind::PlusToken ||
                    tk.kind == SyntaxKind::StarToken ||
                    tk.kind == SyntaxKind::SlashToken ||
                    tk.kind == SyntaxKind::PercentToken) {
                    if (offset > 1) hasTopLevelArithmetic = true;
                } else if (tk.kind == SyntaxKind::MinusToken && offset > 1) {
                    const auto prevKind = peek(offset - 1).kind;
                    if (prevKind == SyntaxKind::IntegerLiteralToken ||
                        prevKind == SyntaxKind::FloatLiteralToken ||
                        prevKind == SyntaxKind::CharLiteralToken ||
                        prevKind == SyntaxKind::IdentifierToken ||
                        prevKind == SyntaxKind::CloseParenToken) {
                        hasTopLevelArithmetic = true;
                    }
                }
            }
            ++offset;
        }
        const auto afterClose = peek(offset + 1).kind;
        if (hasTopLevelArithmetic || isArithmeticContinuation(afterClose) ||
            afterClose == SyntaxKind::DotToken) {
            auto cp = std::make_unique<ConstantPatternSyntax>();
            cp->expression = parseBinaryExpression(4);
            return cp;
        }

        auto paren = std::make_unique<ParenthesizedPatternSyntax>();
        paren->openParenToken = nextToken();
        paren->pattern = parsePattern();
        paren->closeParenToken = match(SyntaxKind::CloseParenToken);
        return paren;
    }

    // 4. Discard pattern '_'
    if (current().kind == SyntaxKind::IdentifierToken &&
        current().text == "_" &&
        peek(1).kind != SyntaxKind::DotToken &&
        peek(1).kind != SyntaxKind::OpenBraceToken &&
        peek(1).kind != SyntaxKind::OpenBracketToken) {
        auto disc = std::make_unique<DiscardPatternSyntax>();
        disc->underscoreToken = nextToken();
        return disc;
    }

    // 5. 'var x' pattern
    if (current().kind == SyntaxKind::IdentifierToken &&
        current().text == "var" &&
        isPatternDesignation(peek(1))) {
        auto vp = std::make_unique<VarPatternSyntax>();
        vp->varKeyword = nextToken();
        vp->designationToken = nextToken();
        return vp;
    }

    // 6. Literals, unary +/- constants, or member-access constants (e.g. Enum.Member, Limits.Max)
    if (current().kind == SyntaxKind::NullKeyword ||
        current().kind == SyntaxKind::TrueKeyword ||
        current().kind == SyntaxKind::FalseKeyword ||
        current().kind == SyntaxKind::IntegerLiteralToken ||
        current().kind == SyntaxKind::FloatLiteralToken ||
        current().kind == SyntaxKind::StringLiteralToken ||
        current().kind == SyntaxKind::CharLiteralToken ||
        current().kind == SyntaxKind::MinusToken ||
        current().kind == SyntaxKind::PlusToken ||
        (current().kind == SyntaxKind::IdentifierToken &&
         (peek(1).kind == SyntaxKind::DotToken ||
          isArithmeticContinuation(peek(1).kind)))) {
        auto cp = std::make_unique<ConstantPatternSyntax>();
        cp->expression = parseBinaryExpression(4);
        return cp;
    }

    // 7. Type pattern, typed property pattern, or bare const-identifier pattern
    if (isPrimitiveTypeKeyword(current().kind) ||
        current().kind == SyntaxKind::IdentifierToken) {
        auto parsedType = parseType();
        if (current().kind == SyntaxKind::OpenBraceToken) {
            return std::make_unique<PropertyPatternSyntax>(
                parsePropertyPatternTail(std::move(parsedType)));
        }
        auto tp = std::make_unique<TypePatternSyntax>();
        tp->type = std::move(parsedType);
        if (isPatternDesignation(current())) {
            tp->designationToken = nextToken();
        }
        return tp;
    }

    // Fallback: parse constant expression for error recovery
    auto cp = std::make_unique<ConstantPatternSyntax>();
    cp->expression = parseBinaryExpression(4);
    return cp;
}

} // namespace realscript::syntax
