// AnimLangParser.h - S-expression Parser for AnimLang DSL
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangAST.h"
#include "AnimLangTokenizer.h"

/**
 * Parse error with source location
 */
struct ANIMBP2FP_API FAnimLangParseError
{
	FString Message;
	int32 Line;
	int32 Column;
	
	FString ToString() const
	{
		return FString::Printf(TEXT("Parse error at %d:%d: %s"), Line, Column, *Message);
	}
};

/**
 * AnimLang S-expression Parser
 * 
 * Grammar (informal):
 *   Program     ::= '(' 'anim-blueprint' STRING TopLevel* ')'
 *   TopLevel    ::= ':skeleton' STRING
 *                 | ':variables' '[' VarDef* ']'
 *                 | '(' 'helpers' HelperGraph* ')'
 *                 | '(' 'define' IDENT NodeExpr ')'
 *                 | ':anim-graph' NodeExpr
 *   HelperGraph ::= '(' 'helper-graph' HelperField* ')'
 *   HelperField ::= ':id' STRING
 *                 | ':graph-name' STRING
 *                 | ':generated-var' STRING
 *                 | ':generated-type' IDENT
 *                 | ':update-group' STRING
 *                 | ':dsl' RawExpr
 *   VarDef      ::= '(' TypeName ':name' STRING VarField* ')'
 *                 | '(' TypeName ':' IDENT VarField* ')'       -- legacy short name
 *   NodeExpr    ::= '(' NodeType Property* PoseInput* ')'
 *                 | IDENT                                    -- variable reference (UseCachedPose)
 *   Property    ::= ':' KEY Value
 *   PoseInput   ::= ':' KEY NodeExpr
 *   Value       ::= STRING | NUMBER | BOOL | '(' 'ref' STRING ')' | '(' 'asset' STRING ')' | '[' Value* ']' | IDENT
 *   RawExpr     ::= any balanced S-expression / array / scalar, reconstructed canonically as text

 * 
 * The parser auto-detects whether a :key is followed by a node expression (→ named child)
 * or a literal value (→ property).
 */
class ANIMBP2FP_API FAnimLangParser
{
public:
	/**
	 * Parse DSL source code into an AST
	 * @param Source  The full .animlang source (including file header comments)
	 * @param OutErrors  Receives parse errors
	 * @return Parsed AST, or nullptr on fatal error
	 */
	static TSharedPtr<FAnimGraphAST> Parse(const FString& Source, TArray<FAnimLangParseError>& OutErrors);
	
	/**
	 * Parse from pre-tokenized token stream
	 * @param Tokens  Token array (must end with EOF)
	 * @param OutErrors  Receives parse errors
	 * @return Parsed AST, or nullptr on fatal error
	 */
	static TSharedPtr<FAnimGraphAST> ParseTokens(const TArray<FAnimLangToken>& Tokens, TArray<FAnimLangParseError>& OutErrors);

private:
	const TArray<FAnimLangToken>& Tokens;
	int32 Pos;
	TArray<FAnimLangParseError>& Errors;
	
	FAnimLangParser(const TArray<FAnimLangToken>& InTokens, TArray<FAnimLangParseError>& InErrors);
	
	// Token access
	const FAnimLangToken& Current() const;
	const FAnimLangToken& Peek(int32 Ahead = 0) const;
	const FAnimLangToken& Advance();
	bool IsAtEnd() const;
	bool Check(EAnimLangTokenType Type) const;
	bool CheckValue(EAnimLangTokenType Type, const FString& Value) const;
	bool Match(EAnimLangTokenType Type);
	bool Expect(EAnimLangTokenType Type, const FString& Context);
	
	// Error handling
	void Error(const FString& Message);
	void ErrorAt(const FAnimLangToken& Token, const FString& Message);
	void Synchronize();  // Skip to next meaningful position after error
	
	// Parsing rules
	TSharedPtr<FAnimGraphAST> ParseProgram();
	void ParseTopLevel(TSharedPtr<FAnimGraphAST> AST);
	void ParseVariables(TSharedPtr<FAnimGraphAST> AST);
	FVariableDef ParseVarDef();
	void ParseHelpers(TSharedPtr<FAnimGraphAST> AST);
	FHelperGraphDef ParseHelperGraphDef();
	void ParseLogicGraphs(TSharedPtr<FAnimGraphAST> AST);
	FLogicGraphDef ParseLogicGraphDef();
	void ParseAnimationLayers(TSharedPtr<FAnimGraphAST> AST);
	FAnimationLayerDef ParseAnimationLayerDef();
	void ParseMetadata(TSharedPtr<FAnimGraphAST> AST);
	void ParseDependencies(TSharedPtr<FAnimGraphAST> AST);
	FAnimDependency ParseDependency();
	FAnimationAssetMetadataSnapshot ParseAnimationAssetMetadata();
	FExternalAssetTypedSnapshot ParseExternalAssetTypedSnapshot();
	void ParseDefine(TSharedPtr<FAnimGraphAST> AST);
	TSharedPtr<FAnimNodeAST> ParseNodeExpr();
	TSharedPtr<FAnimNodeAST> ParseNodeBody();  // Inside parentheses, after node type
	FString ParseValue();  // Parse a property value (literal, ref, asset, etc.)
	FString ParseTransitionList();  // Parse [...] transition syntax
	FString ParseRawExpressionText();  // Parse any balanced raw expression and reconstruct it canonically

	
	// Helpers
	bool IsValueStart() const;
	bool IsNodeExprStart() const;
};
