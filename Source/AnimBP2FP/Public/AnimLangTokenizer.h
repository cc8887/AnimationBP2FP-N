// AnimLangTokenizer.h - S-expression Lexer for AnimLang DSL
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangDiagnostics.h"

struct FAnimLangParseError;

/**
 * Token 类型枚举
 */
enum class EAnimLangTokenType : uint8
{
	// Structural
	LParen,        // (
	RParen,        // )
	LBracket,      // [
	RBracket,      // ]
	
	// Literals
	String,        // "hello"
	Integer,       // 123, -5
	Float,         // 0.5, -3.14
	Bool,          // true, false
	
	// Identifiers & Keywords
	Keyword,       // :name, :loop, :initial
	Identifier,    // sequence-player, blend, Post-Layering
	
	// Special
	Arrow,         // ->
	
	// Meta
	Comment,       // ;; ...
	EndOfFile,
	Error          // Lexer error
};

/**
 * 单个 Token
 */
struct ANIMBP2FP_API FAnimLangToken
{
	EAnimLangTokenType Type;
	FString Value;        // Raw token value (without quotes for strings, without : for keywords)
	int32 Line;           // 1-based line number
	int32 Column;         // 1-based column number
	int32 Offset;         // 0-based character offset in source
	FAnimLangSourceLoc Span;
	
	FAnimLangToken()
		: Type(EAnimLangTokenType::Error), Line(0), Column(0), Offset(0)
	{
	}
	
	FAnimLangToken(EAnimLangTokenType InType, const FString& InValue, int32 InLine, int32 InCol, int32 InOffset)
		: Type(InType), Value(InValue), Line(InLine), Column(InCol), Offset(InOffset)
	{
		Span.Line = InLine;
		Span.Column = InCol;
		Span.Offset = InOffset;
	}
	
	/** Human-readable token type name */
	static FString TypeToString(EAnimLangTokenType T);
	
	/** Format as "Type(Value) at Line:Col" for debugging */
	FString ToString() const;
	
	bool IsLiteral() const
	{
		return Type == EAnimLangTokenType::String
			|| Type == EAnimLangTokenType::Integer
			|| Type == EAnimLangTokenType::Float
			|| Type == EAnimLangTokenType::Bool;
	}
};

/**
 * Lexer error
 */
struct FAnimLangLexError
{
	FString Message;
	int32 Line;
	int32 Column;
	
	FString ToString() const
	{
		return FString::Printf(TEXT("Lex error at %d:%d: %s"), Line, Column, *Message);
	}
};

/**
 * AnimLang S-expression Tokenizer
 * 
 * Converts raw DSL text into a stream of tokens.
 * Supports:
 *   - S-expression parentheses ( ) [ ]
 *   - String literals with escape sequences: "hello \"world\""
 *   - Numbers: 123, -5, 0.5, -3.14, 1e10
 *   - Booleans: true, false
 *   - Keywords: :name, :loop, :initial
 *   - Identifiers: sequence-player, blend, Post-Layering
 *   - Arrow: ->
 *   - Comments: ;; line comment
 */
class ANIMBP2FP_API FAnimLangTokenizer
{
public:
	/**
	 * Tokenize the entire source string
	 * @param Source  The DSL source code
	 * @param OutTokens  Receives the token list (excludes comments by default)
	 * @param OutErrors  Receives any lexer errors
	 * @param bKeepComments  If true, comment tokens are included in output
	 * @return true if no errors occurred
	 */
	static bool Tokenize(const FString& Source, TArray<FAnimLangToken>& OutTokens, TArray<FAnimLangLexError>& OutErrors, bool bKeepComments = false);

	/** Tokenize with source-file spans and parse-compatible errors. */
	static TArray<FAnimLangToken> Tokenize(
		const FString& Source,
		const FString& SourceFile,
		TArray<FAnimLangParseError>* OutErrors = nullptr);

private:
	// Internal state
	const TCHAR* Src;
	int32 Pos;
	int32 Len;
	int32 Line;
	int32 Col;
	FString SourceFile;
	
	FAnimLangTokenizer(const FString& Source, const FString& InSourceFile);
	static bool TokenizeInternal(
		const FString& Source,
		const FString& SourceFile,
		TArray<FAnimLangToken>& OutTokens,
		TArray<FAnimLangLexError>& OutErrors,
		bool bKeepComments);
	
	// Character helpers
	TCHAR Peek() const;
	TCHAR PeekAt(int32 Ahead) const;
	TCHAR Advance();
	bool IsAtEnd() const;
	void SkipWhitespace();
	
	// Token scanners
	FAnimLangToken ScanToken(TArray<FAnimLangLexError>& OutErrors);
	FAnimLangToken ScanString(TArray<FAnimLangLexError>& OutErrors);
	FAnimLangToken ScanNumber();
	FAnimLangToken ScanKeyword();
	FAnimLangToken ScanIdentifierOrBool();
	FAnimLangToken ScanComment();
	
	// Helpers
	FAnimLangToken MakeToken(EAnimLangTokenType Type, const FString& Value, int32 StartLine, int32 StartCol, int32 StartOffset) const;
	bool IsIdentChar(TCHAR Ch) const;
	bool IsDigit(TCHAR Ch) const;
};
