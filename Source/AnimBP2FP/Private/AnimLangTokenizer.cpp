// AnimLangTokenizer.cpp - S-expression Lexer Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangTokenizer.h"
#include "AnimLangParser.h"

// ========== FAnimLangToken ==========

FString FAnimLangToken::TypeToString(EAnimLangTokenType T)
{
	switch (T)
	{
	case EAnimLangTokenType::LParen:     return TEXT("LParen");
	case EAnimLangTokenType::RParen:     return TEXT("RParen");
	case EAnimLangTokenType::LBracket:   return TEXT("LBracket");
	case EAnimLangTokenType::RBracket:   return TEXT("RBracket");
	case EAnimLangTokenType::String:     return TEXT("String");
	case EAnimLangTokenType::Integer:    return TEXT("Integer");
	case EAnimLangTokenType::Float:      return TEXT("Float");
	case EAnimLangTokenType::Bool:       return TEXT("Bool");
	case EAnimLangTokenType::Keyword:    return TEXT("Keyword");
	case EAnimLangTokenType::Identifier: return TEXT("Identifier");
	case EAnimLangTokenType::Arrow:      return TEXT("Arrow");
	case EAnimLangTokenType::Comment:    return TEXT("Comment");
	case EAnimLangTokenType::EndOfFile:  return TEXT("EOF");
	case EAnimLangTokenType::Error:      return TEXT("Error");
	default:                             return TEXT("Unknown");
	}
}

FString FAnimLangToken::ToString() const
{
	return FString::Printf(TEXT("%s(%s) at %d:%d"), *TypeToString(Type), *Value, Line, Column);
}

// ========== FAnimLangTokenizer ==========

FAnimLangTokenizer::FAnimLangTokenizer(const FString& Source, const FString& InSourceFile)
	: Src(*Source)
	, Pos(0)
	, Len(Source.Len())
	, Line(1)
	, Col(1)
	, SourceFile(InSourceFile)
{
}

TCHAR FAnimLangTokenizer::Peek() const
{
	return Pos < Len ? Src[Pos] : 0;
}

TCHAR FAnimLangTokenizer::PeekAt(int32 Ahead) const
{
	int32 Idx = Pos + Ahead;
	return Idx < Len ? Src[Idx] : 0;
}

TCHAR FAnimLangTokenizer::Advance()
{
	if (Pos >= Len) return 0;
	TCHAR Ch = Src[Pos++];
	if (Ch == '\n')
	{
		Line++;
		Col = 1;
	}
	else
	{
		Col++;
	}
	return Ch;
}

bool FAnimLangTokenizer::IsAtEnd() const
{
	return Pos >= Len;
}

void FAnimLangTokenizer::SkipWhitespace()
{
	while (!IsAtEnd())
	{
		TCHAR Ch = Peek();
		if (Ch == ' ' || Ch == '\t' || Ch == '\r' || Ch == '\n')
		{
			Advance();
		}
		else
		{
			break;
		}
	}
}

bool FAnimLangTokenizer::IsIdentChar(TCHAR Ch) const
{
	// Identifiers can contain letters, digits, hyphens, underscores, dots, and some special chars
	return FChar::IsAlnum(Ch) || Ch == '-' || Ch == '_' || Ch == '.' || Ch == '/' || Ch == '!' || Ch == '?' || Ch == '+' || Ch == '*' || Ch == '=' || Ch == '<' || Ch == '>';
}

bool FAnimLangTokenizer::IsDigit(TCHAR Ch) const
{
	return Ch >= '0' && Ch <= '9';
}

FAnimLangToken FAnimLangTokenizer::MakeToken(EAnimLangTokenType Type, const FString& Value, int32 StartLine, int32 StartCol, int32 StartOffset) const
{
	FAnimLangToken Token(Type, Value, StartLine, StartCol, StartOffset);
	Token.Span.SourceFile = SourceFile;
	Token.Span.Length = Pos - StartOffset;
	return Token;
}

// ========== Main Tokenize ==========

bool FAnimLangTokenizer::Tokenize(const FString& Source, TArray<FAnimLangToken>& OutTokens, TArray<FAnimLangLexError>& OutErrors, bool bKeepComments)
{
	return TokenizeInternal(Source, FString(), OutTokens, OutErrors, bKeepComments);
}

TArray<FAnimLangToken> FAnimLangTokenizer::Tokenize(
	const FString& Source,
	const FString& SourceFile,
	TArray<FAnimLangParseError>* OutErrors)
{
	TArray<FAnimLangToken> Tokens;
	TArray<FAnimLangLexError> LexErrors;
	TokenizeInternal(Source, SourceFile, Tokens, LexErrors, false);

	if (OutErrors != nullptr)
	{
		for (const FAnimLangLexError& LexError : LexErrors)
		{
			FAnimLangParseError& ParseError = OutErrors->AddDefaulted_GetRef();
			ParseError.Message = LexError.Message;
			ParseError.Line = LexError.Line;
			ParseError.Column = LexError.Column;
		}
	}
	return Tokens;
}

bool FAnimLangTokenizer::TokenizeInternal(
	const FString& Source,
	const FString& SourceFile,
	TArray<FAnimLangToken>& OutTokens,
	TArray<FAnimLangLexError>& OutErrors,
	bool bKeepComments)
{
	FAnimLangTokenizer Lexer(Source, SourceFile);
	
	while (!Lexer.IsAtEnd())
	{
		Lexer.SkipWhitespace();
		if (Lexer.IsAtEnd()) break;
		
		FAnimLangToken Token = Lexer.ScanToken(OutErrors);
		
		if (Token.Type == EAnimLangTokenType::Comment && !bKeepComments)
		{
			continue;  // Skip comments unless requested
		}
		
		if (Token.Type == EAnimLangTokenType::Error)
		{
			// Error already recorded in OutErrors, but still add the token
			// so the parser can report position context
			OutTokens.Add(Token);
		}
		else
		{
			OutTokens.Add(Token);
		}
	}
	
	// Always end with EOF
	FAnimLangToken EndToken(EAnimLangTokenType::EndOfFile, TEXT(""), Lexer.Line, Lexer.Col, Lexer.Pos);
	EndToken.Span.SourceFile = SourceFile;
	OutTokens.Add(MoveTemp(EndToken));
	
	return OutErrors.Num() == 0;
}

// ========== Token Scanners ==========

FAnimLangToken FAnimLangTokenizer::ScanToken(TArray<FAnimLangLexError>& OutErrors)
{
	int32 StartLine = Line;
	int32 StartCol = Col;
	int32 StartOffset = Pos;
	
	TCHAR Ch = Peek();
	
	// Single-char tokens
	switch (Ch)
	{
	case '(':
		Advance();
		return MakeToken(EAnimLangTokenType::LParen, TEXT("("), StartLine, StartCol, StartOffset);
	case ')':
		Advance();
		return MakeToken(EAnimLangTokenType::RParen, TEXT(")"), StartLine, StartCol, StartOffset);
	case '[':
		Advance();
		return MakeToken(EAnimLangTokenType::LBracket, TEXT("["), StartLine, StartCol, StartOffset);
	case ']':
		Advance();
		return MakeToken(EAnimLangTokenType::RBracket, TEXT("]"), StartLine, StartCol, StartOffset);
	default:
		break;
	}
	
	// Comment: ;; or ;
	if (Ch == ';')
	{
		return ScanComment();
	}
	
	// String literal
	if (Ch == '"')
	{
		return ScanString(OutErrors);
	}
	
	// Keyword: :identifier
	if (Ch == ':')
	{
		return ScanKeyword();
	}
	
	// Number (including negative)
	if (IsDigit(Ch) || (Ch == '-' && IsDigit(PeekAt(1))))
	{
		// But we need to distinguish "->" from negative numbers
		if (Ch == '-' && PeekAt(1) == '>')
		{
			Advance(); Advance();
			return MakeToken(EAnimLangTokenType::Arrow, TEXT("->"), StartLine, StartCol, StartOffset);
		}
		return ScanNumber();
	}
	
	// Arrow: ->
	if (Ch == '-' && PeekAt(1) == '>')
	{
		Advance(); Advance();
		return MakeToken(EAnimLangTokenType::Arrow, TEXT("->"), StartLine, StartCol, StartOffset);
	}
	
	// Identifier or boolean
	if (FChar::IsAlpha(Ch) || Ch == '_' || Ch == '-' || Ch == '+' || Ch == '*' || Ch == '/' || Ch == '!' || Ch == '?' || Ch == '<' || Ch == '>' || Ch == '=')
	{
		return ScanIdentifierOrBool();
	}
	
	// Unknown character
	Advance();
	FAnimLangLexError Error;
	Error.Message = FString::Printf(TEXT("Unexpected character '%c' (0x%04X)"), Ch, (int32)Ch);
	Error.Line = StartLine;
	Error.Column = StartCol;
	OutErrors.Add(Error);
	return MakeToken(EAnimLangTokenType::Error, FString(1, &Ch), StartLine, StartCol, StartOffset);
}

FAnimLangToken FAnimLangTokenizer::ScanString(TArray<FAnimLangLexError>& OutErrors)
{
	int32 StartLine = Line;
	int32 StartCol = Col;
	int32 StartOffset = Pos;
	
	Advance();  // consume opening "
	
	FString Value;
	while (!IsAtEnd())
	{
		TCHAR Ch = Peek();
		
		if (Ch == '"')
		{
			Advance();  // consume closing "
			return MakeToken(EAnimLangTokenType::String, Value, StartLine, StartCol, StartOffset);
		}
		
		if (Ch == '\\')
		{
			Advance();  // consume backslash
			TCHAR Escaped = Peek();
			if (IsAtEnd())
			{
				FAnimLangLexError Error;
				Error.Message = TEXT("Unterminated string: backslash at end of input");
				Error.Line = Line;
				Error.Column = Col;
				OutErrors.Add(Error);
				return MakeToken(EAnimLangTokenType::Error, Value, StartLine, StartCol, StartOffset);
			}
			
			switch (Escaped)
			{
			case '"':  Value += '"'; break;
			case '\\': Value += '\\'; break;
			case 'n':  Value += '\n'; break;
			case 't':  Value += '\t'; break;
			case 'r':  Value += '\r'; break;
			default:
				Value += '\\';
				Value += Escaped;
				break;
			}
			Advance();
		}
		else if (Ch == '\n')
		{
			// Allow multiline strings
			Value += Ch;
			Advance();
		}
		else
		{
			Value += Ch;
			Advance();
		}
	}
	
	// Unterminated string
	FAnimLangLexError Error;
	Error.Message = TEXT("Unterminated string literal");
	Error.Line = StartLine;
	Error.Column = StartCol;
	OutErrors.Add(Error);
	return MakeToken(EAnimLangTokenType::Error, Value, StartLine, StartCol, StartOffset);
}

FAnimLangToken FAnimLangTokenizer::ScanNumber()
{
	int32 StartLine = Line;
	int32 StartCol = Col;
	int32 StartOffset = Pos;
	
	FString Value;
	bool bIsFloat = false;
	
	// Optional leading minus
	if (Peek() == '-')
	{
		Value += Advance();
	}
	
	// Integer part
	while (!IsAtEnd() && IsDigit(Peek()))
	{
		Value += Advance();
	}
	
	// Decimal part
	if (!IsAtEnd() && Peek() == '.' && IsDigit(PeekAt(1)))
	{
		bIsFloat = true;
		Value += Advance();  // consume '.'
		while (!IsAtEnd() && IsDigit(Peek()))
		{
			Value += Advance();
		}
	}
	
	// Scientific notation (e.g., 1e10, 2.5e-3)
	if (!IsAtEnd() && (Peek() == 'e' || Peek() == 'E'))
	{
		bIsFloat = true;
		Value += Advance();
		if (!IsAtEnd() && (Peek() == '+' || Peek() == '-'))
		{
			Value += Advance();
		}
		while (!IsAtEnd() && IsDigit(Peek()))
		{
			Value += Advance();
		}
	}
	
	return MakeToken(
		bIsFloat ? EAnimLangTokenType::Float : EAnimLangTokenType::Integer,
		Value, StartLine, StartCol, StartOffset
	);
}

FAnimLangToken FAnimLangTokenizer::ScanKeyword()
{
	int32 StartLine = Line;
	int32 StartCol = Col;
	int32 StartOffset = Pos;
	
	Advance();  // consume ':'
	
	FString Value;
	while (!IsAtEnd() && IsIdentChar(Peek()))
	{
		Value += Advance();
	}
	
	if (Value.IsEmpty())
	{
		// Bare colon — treat as error or identifier
		return MakeToken(EAnimLangTokenType::Keyword, TEXT(""), StartLine, StartCol, StartOffset);
	}
	
	return MakeToken(EAnimLangTokenType::Keyword, Value, StartLine, StartCol, StartOffset);
}

FAnimLangToken FAnimLangTokenizer::ScanIdentifierOrBool()
{
	int32 StartLine = Line;
	int32 StartCol = Col;
	int32 StartOffset = Pos;
	
	FString Value;
	while (!IsAtEnd() && IsIdentChar(Peek()))
	{
		Value += Advance();
	}
	
	// Check for booleans
	if (Value == TEXT("true") || Value == TEXT("false"))
	{
		return MakeToken(EAnimLangTokenType::Bool, Value, StartLine, StartCol, StartOffset);
	}
	
	return MakeToken(EAnimLangTokenType::Identifier, Value, StartLine, StartCol, StartOffset);
}

FAnimLangToken FAnimLangTokenizer::ScanComment()
{
	int32 StartLine = Line;
	int32 StartCol = Col;
	int32 StartOffset = Pos;
	
	FString Value;
	
	// Consume all ; chars
	while (!IsAtEnd() && Peek() == ';')
	{
		Value += Advance();
	}
	
	// Consume rest of line
	while (!IsAtEnd() && Peek() != '\n')
	{
		Value += Advance();
	}
	
	return MakeToken(EAnimLangTokenType::Comment, Value.TrimStartAndEnd(), StartLine, StartCol, StartOffset);
}
