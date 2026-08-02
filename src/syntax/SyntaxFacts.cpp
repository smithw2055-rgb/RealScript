#include "realscript/syntax/Syntax.h"

#include <unordered_map>

namespace realscript::syntax {
const char* syntaxKindName(SyntaxKind kind) noexcept {
    switch (kind) {
#define RS_KIND(value) case SyntaxKind::value: return #value
        RS_KIND(BadToken);
        RS_KIND(EndOfFileToken);
        RS_KIND(IdentifierToken);
        RS_KIND(IntegerLiteralToken);
        RS_KIND(FloatLiteralToken);
        RS_KIND(StringLiteralToken);
        RS_KIND(PlusToken);
        RS_KIND(MinusToken);
        RS_KIND(StarToken);
        RS_KIND(SlashToken);
        RS_KIND(PercentToken);
        RS_KIND(BangToken);
        RS_KIND(AmpersandAmpersandToken);
        RS_KIND(PipePipeToken);
        RS_KIND(EqualsToken);
        RS_KIND(EqualsEqualsToken);
        RS_KIND(BangEqualsToken);
        RS_KIND(LessToken);
        RS_KIND(LessOrEqualsToken);
        RS_KIND(GreaterToken);
        RS_KIND(GreaterOrEqualsToken);
        RS_KIND(OpenParenToken);
        RS_KIND(CloseParenToken);
        RS_KIND(OpenBraceToken);
        RS_KIND(CloseBraceToken);
        RS_KIND(OpenBracketToken);
        RS_KIND(CloseBracketToken);
        RS_KIND(CommaToken);
        RS_KIND(DotToken);
        RS_KIND(ColonToken);
        RS_KIND(SemicolonToken);
        RS_KIND(ModuleKeyword);
        RS_KIND(ImportKeyword);
        RS_KIND(ReturnKeyword);
        RS_KIND(IfKeyword);
        RS_KIND(ElseKeyword);
        RS_KIND(WhileKeyword);
        RS_KIND(ForKeyword);
        RS_KIND(ForeachKeyword);
        RS_KIND(InKeyword);
        RS_KIND(DoKeyword);
        RS_KIND(BreakKeyword);
        RS_KIND(ContinueKeyword);
        RS_KIND(SwitchKeyword);
        RS_KIND(CaseKeyword);
        RS_KIND(DefaultKeyword);
        RS_KIND(SequenceKeyword);
        RS_KIND(YieldKeyword);
        RS_KIND(ClassKeyword);
        RS_KIND(StructKeyword);
        RS_KIND(EnumKeyword);
        RS_KIND(InterfaceKeyword);
        RS_KIND(StaticKeyword);
        RS_KIND(GetKeyword);
        RS_KIND(SetKeyword);
        RS_KIND(ThisKeyword);
        RS_KIND(NewKeyword);
        RS_KIND(TrueKeyword);
        RS_KIND(FalseKeyword);
        RS_KIND(NullKeyword);
        RS_KIND(BoolKeyword);
        RS_KIND(ByteKeyword);
        RS_KIND(SByteKeyword);
        RS_KIND(ShortKeyword);
        RS_KIND(UShortKeyword);
        RS_KIND(IntKeyword);
        RS_KIND(UIntKeyword);
        RS_KIND(LongKeyword);
        RS_KIND(ULongKeyword);
        RS_KIND(FloatKeyword);
        RS_KIND(DoubleKeyword);
        RS_KIND(StringKeyword);
        RS_KIND(HandleKeyword);
        RS_KIND(VoidKeyword);
        RS_KIND(CompilationUnit);
        RS_KIND(AttributeList);
        RS_KIND(Attribute);
        RS_KIND(AttributeArgument);
        RS_KIND(ModuleDeclaration);
        RS_KIND(ClassDeclaration);
        RS_KIND(StructDeclaration);
        RS_KIND(EnumDeclaration);
        RS_KIND(InterfaceDeclaration);
        RS_KIND(InterfaceMethodDeclaration);
        RS_KIND(EnumMemberDeclaration);
        RS_KIND(FieldDeclaration);
        RS_KIND(ConstructorDeclaration);
        RS_KIND(PropertyDeclaration);
        RS_KIND(AccessorDeclaration);
        RS_KIND(ImportDeclaration);
        RS_KIND(FunctionDeclaration);
        RS_KIND(SequenceDeclaration);
        RS_KIND(Parameter);
        RS_KIND(TypeName);
        RS_KIND(BlockStatement);
        RS_KIND(ReturnStatement);
        RS_KIND(IfStatement);
        RS_KIND(WhileStatement);
        RS_KIND(ForStatement);
        RS_KIND(ForeachStatement);
        RS_KIND(DoWhileStatement);
        RS_KIND(BreakStatement);
        RS_KIND(ContinueStatement);
        RS_KIND(SwitchSection);
        RS_KIND(SwitchStatement);
        RS_KIND(YieldWaitStatement);
        RS_KIND(VariableDeclarationStatement);
        RS_KIND(ExpressionStatement);
        RS_KIND(LiteralExpression);
        RS_KIND(NameExpression);
        RS_KIND(UnaryExpression);
        RS_KIND(BinaryExpression);
        RS_KIND(AssignmentExpression);
        RS_KIND(MemberAssignmentExpression);
        RS_KIND(ElementAssignmentExpression);
        RS_KIND(ParenthesizedExpression);
        RS_KIND(CallExpression);
        RS_KIND(MemberCallExpression);
        RS_KIND(ThisExpression);
        RS_KIND(MemberAccessExpression);
        RS_KIND(ElementAccessExpression);
        RS_KIND(NewObjectExpression);
        RS_KIND(NewArrayExpression);
#undef RS_KIND
    }
    return "UnknownSyntaxKind";
}

SyntaxKind keywordKind(const std::string& textValue) noexcept {
    static const std::unordered_map<std::string, SyntaxKind> keywords = {
        {"module", SyntaxKind::ModuleKeyword},
        {"import", SyntaxKind::ImportKeyword},
        {"return", SyntaxKind::ReturnKeyword},
        {"if", SyntaxKind::IfKeyword},
        {"else", SyntaxKind::ElseKeyword},
        {"while", SyntaxKind::WhileKeyword},
        {"for", SyntaxKind::ForKeyword},
        {"foreach", SyntaxKind::ForeachKeyword},
        {"in", SyntaxKind::InKeyword},
        {"do", SyntaxKind::DoKeyword},
        {"break", SyntaxKind::BreakKeyword},
        {"continue", SyntaxKind::ContinueKeyword},
        {"switch", SyntaxKind::SwitchKeyword},
        {"case", SyntaxKind::CaseKeyword},
        {"default", SyntaxKind::DefaultKeyword},
        {"sequence", SyntaxKind::SequenceKeyword},
        {"yield", SyntaxKind::YieldKeyword},
        {"class", SyntaxKind::ClassKeyword},
        {"struct", SyntaxKind::StructKeyword},
        {"enum", SyntaxKind::EnumKeyword},
        {"interface", SyntaxKind::InterfaceKeyword},
        {"static", SyntaxKind::StaticKeyword},
        {"get", SyntaxKind::GetKeyword},
        {"set", SyntaxKind::SetKeyword},
        {"this", SyntaxKind::ThisKeyword},
        {"new", SyntaxKind::NewKeyword},
        {"true", SyntaxKind::TrueKeyword},
        {"false", SyntaxKind::FalseKeyword},
        {"null", SyntaxKind::NullKeyword},
        {"bool", SyntaxKind::BoolKeyword},
        {"byte", SyntaxKind::ByteKeyword},
        {"sbyte", SyntaxKind::SByteKeyword},
        {"short", SyntaxKind::ShortKeyword},
        {"ushort", SyntaxKind::UShortKeyword},
        {"int", SyntaxKind::IntKeyword},
        {"uint", SyntaxKind::UIntKeyword},
        {"long", SyntaxKind::LongKeyword},
        {"ulong", SyntaxKind::ULongKeyword},
        {"float", SyntaxKind::FloatKeyword},
        {"double", SyntaxKind::DoubleKeyword},
        {"string", SyntaxKind::StringKeyword},
        {"handle", SyntaxKind::HandleKeyword},
        {"void", SyntaxKind::VoidKeyword},
    };
    const auto it = keywords.find(textValue);
    return it == keywords.end() ? SyntaxKind::IdentifierToken : it->second;
}

int unaryPrecedence(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::PlusToken:
    case SyntaxKind::MinusToken:
    case SyntaxKind::BangToken:
        return 7;
    default:
        return 0;
    }
}

int binaryPrecedence(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::StarToken:
    case SyntaxKind::SlashToken:
    case SyntaxKind::PercentToken:
        return 6;
    case SyntaxKind::PlusToken:
    case SyntaxKind::MinusToken:
        return 5;
    case SyntaxKind::LessToken:
    case SyntaxKind::LessOrEqualsToken:
    case SyntaxKind::GreaterToken:
    case SyntaxKind::GreaterOrEqualsToken:
        return 4;
    case SyntaxKind::EqualsEqualsToken:
    case SyntaxKind::BangEqualsToken:
        return 3;
    case SyntaxKind::AmpersandAmpersandToken:
        return 2;
    case SyntaxKind::PipePipeToken:
        return 1;
    default:
        return 0;
    }
}

bool isPrimitiveTypeKeyword(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::BoolKeyword:
    case SyntaxKind::ByteKeyword:
    case SyntaxKind::SByteKeyword:
    case SyntaxKind::ShortKeyword:
    case SyntaxKind::UShortKeyword:
    case SyntaxKind::IntKeyword:
    case SyntaxKind::UIntKeyword:
    case SyntaxKind::LongKeyword:
    case SyntaxKind::ULongKeyword:
    case SyntaxKind::FloatKeyword:
    case SyntaxKind::DoubleKeyword:
    case SyntaxKind::StringKeyword:
    case SyntaxKind::HandleKeyword:
    case SyntaxKind::VoidKeyword:
        return true;
    default:
        return false;
    }
}


} // namespace realscript::syntax
