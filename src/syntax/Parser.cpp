#include "realscript/syntax/Syntax.h"

namespace realscript::syntax {
namespace {

bool isIdentifierLike(SyntaxKind kind) noexcept {
    return kind == SyntaxKind::IdentifierToken || isPrimitiveTypeKeyword(kind);
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
    if (current().kind == SyntaxKind::ModuleKeyword) {
        result.moduleDeclaration = parseModuleDeclaration();
    }
    while (current().kind == SyntaxKind::ImportKeyword) {
        result.imports.push_back(parseImportDeclaration());
    }
    while (current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        auto attributes = parseAttributeLists();
        if (current().kind == SyntaxKind::ClassKeyword) {
            result.classes.push_back(
                parseClassDeclaration(std::move(attributes)));
        } else if (current().kind == SyntaxKind::StructKeyword) {
            result.structs.push_back(
                parseStructDeclaration(std::move(attributes)));
        } else if (current().kind == SyntaxKind::EnumKeyword) {
            result.enums.push_back(
                parseEnumDeclaration(std::move(attributes)));
        } else if (current().kind == SyntaxKind::InterfaceKeyword) {
            result.interfaces.push_back(
                parseInterfaceDeclaration(std::move(attributes)));
        } else if (current().kind == SyntaxKind::DelegateKeyword) {
            result.delegates.push_back(
                parseDelegateDeclaration(std::move(attributes)));
        } else {
            result.functions.push_back(parseFunctionDeclaration(
                std::nullopt,
                std::nullopt,
                std::nullopt,
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
        peek(1).kind == SyntaxKind::EqualsToken) {
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

TypeSyntax Parser::parseType() {
    TypeSyntax result;
    if (isIdentifierLike(current().kind)) {
        result.name = nextToken();
    } else {
        result.name = match(SyntaxKind::IdentifierToken);
    }
    parseTypeArgumentList(
        result.lessToken,
        result.typeArguments,
        result.typeArgumentCommaTokens,
        result.greaterToken);
    if (current().kind == SyntaxKind::OpenBracketToken &&
        peek(1).kind == SyntaxKind::CloseBracketToken) {
        result.openBracketToken = nextToken();
        result.closeBracketToken = nextToken();
    }
    return result;
}

ParameterSyntax Parser::parseParameter() {
    ParameterSyntax result;
    if (current().kind == SyntaxKind::RefKeyword ||
        current().kind == SyntaxKind::OutKeyword ||
        current().kind == SyntaxKind::InKeyword) {
        result.modifierToken = nextToken();
    }
    result.type = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    return result;
}

FieldDeclarationSyntax Parser::parseFieldDeclaration(
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes) {
    FieldDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.type = std::move(type);
    result.identifierToken = std::move(identifier);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

void Parser::parseArgumentList(
    std::vector<std::unique_ptr<ExpressionSyntax>>& arguments,
    std::vector<std::optional<SyntaxToken>>* argumentModifiers,
    std::vector<SyntaxToken>& commaTokens,
    SyntaxToken& closeParenToken) {
    const auto parseArgument = [&]() {
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
    std::vector<AttributeListSyntax> attributes) {
    SequenceDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.sequenceKeyword = match(SyntaxKind::SequenceKeyword);
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
    std::vector<AttributeListSyntax> attributes) {
    DelegateDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.delegateKeyword = match(SyntaxKind::DelegateKeyword);
    result.returnType = parseType();
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
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

EventDeclarationSyntax Parser::parseEventDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    EventDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.eventKeyword = match(SyntaxKind::EventKeyword);
    result.delegateType = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    ConstructorDeclarationSyntax result;
    result.attributes = std::move(attributes);
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
    std::optional<SyntaxToken> staticKeyword,
    TypeSyntax type,
    SyntaxToken identifier,
    std::vector<AttributeListSyntax> attributes) {
    PropertyDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.staticKeyword = std::move(staticKeyword);
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
    return result;
}

FunctionDeclarationSyntax Parser::parseFunctionDeclaration(
    std::optional<SyntaxToken> staticKeyword,
    std::optional<TypeSyntax> returnType,
    std::optional<SyntaxToken> identifier,
    std::vector<AttributeListSyntax> attributes) {
    FunctionDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.staticKeyword = std::move(staticKeyword);
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
    result.body = parseBlockStatement();
    return result;
}

ClassDeclarationSyntax Parser::parseClassDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    ClassDeclarationSyntax result;
    result.attributes = std::move(attributes);
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
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        auto memberAttributes = parseAttributeLists();
        if (current().kind == SyntaxKind::EventKeyword) {
            result.events.push_back(parseEventDeclaration(
                std::move(memberAttributes)));
            continue;
        }
        std::optional<SyntaxToken> staticKeyword;
        if (current().kind == SyntaxKind::StaticKeyword) {
            staticKeyword = nextToken();
        }
        if (!staticKeyword &&
            current().kind == SyntaxKind::SequenceKeyword) {
            result.sequences.push_back(parseSequenceDeclaration(
                std::move(memberAttributes)));
        } else if (!staticKeyword && current().kind == SyntaxKind::IdentifierToken &&
            current().text == typeName && peek(1).kind == SyntaxKind::OpenParenToken) {
            result.constructors.push_back(parseConstructorDeclaration(
                std::move(memberAttributes)));
        } else {
            auto type = parseType();
            auto identifier = match(SyntaxKind::IdentifierToken);
            if (current().kind == SyntaxKind::OpenParenToken) {
                result.methods.push_back(parseFunctionDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
            } else if (current().kind == SyntaxKind::OpenBraceToken) {
                result.properties.push_back(parsePropertyDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
            } else {
                if (staticKeyword) {
                    diagnostics_.report("RS1106", "static fields are not supported", staticKeyword->span);
                }
                result.fields.push_back(parseFieldDeclaration(
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
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
    std::vector<AttributeListSyntax> attributes) {
    StructDeclarationSyntax result;
    result.attributes = std::move(attributes);
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
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        auto memberAttributes = parseAttributeLists();
        if (current().kind == SyntaxKind::EventKeyword) {
            result.events.push_back(parseEventDeclaration(
                std::move(memberAttributes)));
            continue;
        }
        std::optional<SyntaxToken> staticKeyword;
        if (current().kind == SyntaxKind::StaticKeyword) staticKeyword = nextToken();
        if (!staticKeyword &&
            current().kind == SyntaxKind::SequenceKeyword) {
            result.sequences.push_back(parseSequenceDeclaration(
                std::move(memberAttributes)));
        } else if (!staticKeyword && current().kind == SyntaxKind::IdentifierToken &&
            current().text == typeName && peek(1).kind == SyntaxKind::OpenParenToken) {
            result.constructors.push_back(parseConstructorDeclaration(
                std::move(memberAttributes)));
        } else {
            auto type = parseType();
            auto identifier = match(SyntaxKind::IdentifierToken);
            if (current().kind == SyntaxKind::OpenParenToken) {
                result.methods.push_back(parseFunctionDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
            } else if (current().kind == SyntaxKind::OpenBraceToken) {
                result.properties.push_back(parsePropertyDeclaration(
                    std::move(staticKeyword),
                    std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
            } else {
                if (staticKeyword) diagnostics_.report("RS1106", "static fields are not supported", staticKeyword->span);
                result.fields.push_back(parseFieldDeclaration(std::move(type),
                    std::move(identifier),
                    std::move(memberAttributes)));
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
    std::vector<AttributeListSyntax> attributes) {
    InterfaceMethodDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.returnType = parseType();
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
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

InterfaceDeclarationSyntax Parser::parseInterfaceDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    InterfaceDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.interfaceKeyword = match(SyntaxKind::InterfaceKeyword);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        result.methods.push_back(parseInterfaceMethodDeclaration(
            parseAttributeLists()));
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
    std::vector<AttributeListSyntax> attributes) {
    EnumDeclarationSyntax result;
    result.attributes = std::move(attributes);
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

BlockStatementSyntax Parser::parseBlockStatement() {
    BlockStatementSyntax result;
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);

    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        result.statements.push_back(parseStatement());
        if (before == position_) {
            diagnostics_.report("RS1102", "parser made no progress while reading a statement", current().span);
            nextToken();
        }
    }

    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseStatement() {
    if (current().kind == SyntaxKind::IdentifierToken &&
        (peek(1).kind == SyntaxKind::PlusEqualsToken ||
         peek(1).kind == SyntaxKind::MinusEqualsToken)) {
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
    case SyntaxKind::YieldKeyword:
        return parseYieldWaitStatement();
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
    result->type = parseType();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
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
            section.label = parseExpression();
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
        while (current().kind != SyntaxKind::CaseKeyword &&
               current().kind != SyntaxKind::DefaultKeyword &&
               current().kind != SyntaxKind::CloseBraceToken &&
               current().kind != SyntaxKind::EndOfFileToken) {
            section.statements.push_back(parseStatement());
        }
        result->sections.push_back(std::move(section));
    }
    result->closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseYieldWaitStatement() {
    auto result = std::make_unique<YieldWaitStatementSyntax>();
    result->yieldKeyword = match(SyntaxKind::YieldKeyword);
    result->waitTicksToken = match(SyntaxKind::IdentifierToken);
    if (result->waitTicksToken.text != "wait_ticks") {
        diagnostics_.report(
            "RS1113",
            "yield currently supports only wait_ticks(expression)",
            result->waitTicksToken.span);
    }
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->delay = parseExpression();
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

std::unique_ptr<StatementSyntax> Parser::parseVariableDeclarationStatement() {
    auto result = std::make_unique<VariableDeclarationStatementSyntax>();
    result->type = parseType();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    if (current().kind == SyntaxKind::EqualsToken) {
        result->equalsToken = nextToken();
        result->initializer = parseExpression();
    }
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseExpressionStatement() {
    auto result = std::make_unique<ExpressionStatementSyntax>();
    result->expression = parseExpression();
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<ExpressionSyntax> Parser::parseExpression() {
    return parseAssignmentExpression();
}

std::unique_ptr<ExpressionSyntax> Parser::parseAssignmentExpression() {
    auto left = parseBinaryExpression();
    if (current().kind != SyntaxKind::EqualsToken) {
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
        const auto precedence = binaryPrecedence(current().kind);
        if (precedence == 0 || precedence <= parentPrecedence) {
            break;
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
        if (current().kind == SyntaxKind::DotToken) {
            const auto dot = nextToken();
            const auto name = match(SyntaxKind::IdentifierToken);
            std::optional<SyntaxToken> lessToken;
            std::vector<TypeSyntax> typeArguments;
            std::vector<SyntaxToken> typeArgumentCommas;
            std::optional<SyntaxToken> greaterToken;
            parseTypeArgumentList(
                lessToken, typeArguments, typeArgumentCommas,
                greaterToken);
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
                    call->closeParenToken);
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

    if (current().kind == SyntaxKind::OpenParenToken) {
        auto result = std::make_unique<NewObjectExpressionSyntax>();
        result->newKeyword = newKeyword;
        result->type = std::move(type);
        result->openParenToken = nextToken();
        parseArgumentList(
            result->arguments, nullptr, result->commaTokens,
            result->closeParenToken);
        return result;
    }

    auto result = std::make_unique<NewArrayExpressionSyntax>();
    result->newKeyword = newKeyword;
    result->elementType = std::move(type);
    result->openBracketToken = match(SyntaxKind::OpenBracketToken);
    result->length = parseExpression();
    result->closeBracketToken = match(SyntaxKind::CloseBracketToken);
    return result;
}

std::unique_ptr<ExpressionSyntax> Parser::parsePrimaryExpression() {
    switch (current().kind) {
    case SyntaxKind::NewKeyword:
        return parseNewExpression();
    case SyntaxKind::ThisKeyword: {
        auto result = std::make_unique<ThisExpressionSyntax>();
        result->thisKeyword = nextToken();
        return result;
    }
    case SyntaxKind::OpenParenToken: {
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
    case SyntaxKind::StringLiteralToken: {
        auto result = std::make_unique<LiteralExpressionSyntax>();
        result->literalToken = nextToken();
        return result;
    }
    case SyntaxKind::IdentifierToken:
        if (peek(1).kind == SyntaxKind::OpenParenToken ||
            isGenericCallStart()) {
            return parseCallExpression();
        } else {
            auto result = std::make_unique<NameExpressionSyntax>();
            result->identifierToken = nextToken();
            return result;
        }
    default: {
        auto result = std::make_unique<NameExpressionSyntax>();
        result->identifierToken = match(SyntaxKind::IdentifierToken);
        return result;
    }
    }
}

bool Parser::isGenericCallStart() const noexcept {
    if (current().kind != SyntaxKind::IdentifierToken ||
        peek(1).kind != SyntaxKind::LessToken) {
        return false;
    }
    int depth = 0;
    std::size_t offset = 1;
    while (position_ + offset < tokens_.size()) {
        const auto kind = peek(offset).kind;
        if (kind == SyntaxKind::LessToken) ++depth;
        else if (kind == SyntaxKind::GreaterToken) {
            --depth;
            if (depth == 0) {
                return peek(offset + 1).kind ==
                    SyntaxKind::OpenParenToken;
            }
        } else if (kind == SyntaxKind::EndOfFileToken ||
                   kind == SyntaxKind::SemicolonToken) {
            return false;
        }
        ++offset;
    }
    return false;
}

std::unique_ptr<ExpressionSyntax> Parser::parseCallExpression() {
    auto result = std::make_unique<CallExpressionSyntax>();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    parseTypeArgumentList(
        result->lessToken,
        result->typeArguments,
        result->typeArgumentCommaTokens,
        result->greaterToken);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    parseArgumentList(
        result->arguments, &result->argumentModifiers,
        result->commaTokens, result->closeParenToken);
    return result;
}

bool Parser::isVariableDeclarationStart() const noexcept {
    if (!isIdentifierLike(current().kind)) {
        return false;
    }
    std::size_t offset = 1;
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
    if (peek(offset).kind == SyntaxKind::OpenBracketToken &&
        peek(offset + 1).kind == SyntaxKind::CloseBracketToken) {
        offset += 2;
    }
    return peek(offset).kind == SyntaxKind::IdentifierToken &&
        (peek(offset + 1).kind == SyntaxKind::EqualsToken ||
         peek(offset + 1).kind == SyntaxKind::SemicolonToken);
}

} // namespace realscript::syntax
