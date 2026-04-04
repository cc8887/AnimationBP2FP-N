// AnimLangParser.cpp - S-expression Parser Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangParser.h"

// Sentinel token for out-of-bounds access
static const FAnimLangToken GEOFToken(EAnimLangTokenType::EndOfFile, TEXT(""), 0, 0, 0);

// Helper: re-escape a string value for embedding back into DSL (re-adds \" around any inner quotes)
static FString EscapeStringForDSL(const FString& Value)
{
	FString Esc = Value;
	Esc.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Esc.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Esc.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Esc.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	return FString::Printf(TEXT("\"%s\""), *Esc);
}

// ========== Construction & Token Access ==========

FAnimLangParser::FAnimLangParser(const TArray<FAnimLangToken>& InTokens, TArray<FAnimLangParseError>& InErrors)
	: Tokens(InTokens)
	, Pos(0)
	, Errors(InErrors)
{
}

const FAnimLangToken& FAnimLangParser::Current() const
{
	return Pos < Tokens.Num() ? Tokens[Pos] : GEOFToken;
}

const FAnimLangToken& FAnimLangParser::Peek(int32 Ahead) const
{
	int32 Idx = Pos + Ahead;
	return Idx < Tokens.Num() ? Tokens[Idx] : GEOFToken;
}

const FAnimLangToken& FAnimLangParser::Advance()
{
	const FAnimLangToken& Tok = Current();
	if (Pos < Tokens.Num())
	{
		Pos++;
	}
	return Tok;
}

bool FAnimLangParser::IsAtEnd() const
{
	return Current().Type == EAnimLangTokenType::EndOfFile;
}

bool FAnimLangParser::Check(EAnimLangTokenType Type) const
{
	return Current().Type == Type;
}

bool FAnimLangParser::CheckValue(EAnimLangTokenType Type, const FString& Value) const
{
	return Current().Type == Type && Current().Value == Value;
}

bool FAnimLangParser::Match(EAnimLangTokenType Type)
{
	if (Check(Type))
	{
		Advance();
		return true;
	}
	return false;
}

bool FAnimLangParser::Expect(EAnimLangTokenType Type, const FString& Context)
{
	if (Check(Type))
	{
		Advance();
		return true;
	}
	
	Error(FString::Printf(TEXT("Expected %s in %s, got %s(%s)"),
		*FAnimLangToken::TypeToString(Type),
		*Context,
		*FAnimLangToken::TypeToString(Current().Type),
		*Current().Value));
	return false;
}

// ========== Error Handling ==========

void FAnimLangParser::Error(const FString& Message)
{
	ErrorAt(Current(), Message);
}

void FAnimLangParser::ErrorAt(const FAnimLangToken& Token, const FString& Message)
{
	FAnimLangParseError Err;
	Err.Message = Message;
	Err.Line = Token.Line;
	Err.Column = Token.Column;
	Errors.Add(Err);
}

void FAnimLangParser::Synchronize()
{
	// Skip tokens until we find a reasonable restart point
	int32 Depth = 0;
	while (!IsAtEnd())
	{
		if (Check(EAnimLangTokenType::LParen))
		{
			Depth++;
		}
		else if (Check(EAnimLangTokenType::RParen))
		{
			if (Depth <= 0)
			{
				return;  // Don't consume the RParen; let the caller handle it
			}
			Depth--;
		}
		Advance();
	}
}

// ========== Main Parse Entry Points ==========

TSharedPtr<FAnimGraphAST> FAnimLangParser::Parse(const FString& Source, TArray<FAnimLangParseError>& OutErrors)
{
	// First tokenize
	TArray<FAnimLangToken> Tokens;
	TArray<FAnimLangLexError> LexErrors;
	
	FAnimLangTokenizer::Tokenize(Source, Tokens, LexErrors, false);
	
	// Convert lex errors to parse errors
	for (const FAnimLangLexError& LexErr : LexErrors)
	{
		FAnimLangParseError ParseErr;
		ParseErr.Message = LexErr.Message;
		ParseErr.Line = LexErr.Line;
		ParseErr.Column = LexErr.Column;
		OutErrors.Add(ParseErr);
	}
	
	if (LexErrors.Num() > 0)
	{
		// Still try to parse — lexer errors are non-fatal
	}
	
	return ParseTokens(Tokens, OutErrors);
}

TSharedPtr<FAnimGraphAST> FAnimLangParser::ParseTokens(const TArray<FAnimLangToken>& InTokens, TArray<FAnimLangParseError>& OutErrors)
{
	FAnimLangParser Parser(InTokens, OutErrors);
	return Parser.ParseProgram();
}

// ========== Grammar Rules ==========

// Program ::= '(' 'anim-blueprint' STRING TopLevel* ')'
TSharedPtr<FAnimGraphAST> FAnimLangParser::ParseProgram()
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	
	if (!Expect(EAnimLangTokenType::LParen, TEXT("program")))
	{
		return nullptr;
	}
	
	// Expect "anim-blueprint" identifier
	if (!CheckValue(EAnimLangTokenType::Identifier, TEXT("anim-blueprint")))
	{
		Error(FString::Printf(TEXT("Expected 'anim-blueprint', got '%s'"), *Current().Value));
		return nullptr;
	}
	Advance();
	
	// Blueprint name (string)
	if (!Check(EAnimLangTokenType::String))
	{
		Error(TEXT("Expected blueprint name string"));
		return nullptr;
	}
	AST->Name = Advance().Value;
	
	// Parse top-level elements until closing ')'
	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		ParseTopLevel(AST);
		
		// Safety: if we haven't advanced, break to avoid infinite loop
		// This shouldn't happen if all parse functions consume at least one token or error out
	}
	
	Expect(EAnimLangTokenType::RParen, TEXT("anim-blueprint"));
	
	return AST;
}

// TopLevel ::= ':skeleton' STRING
//            | ':variables' '[' VarDef* ']'
//            | '(' 'define' IDENT NodeExpr ')'
//            | ':anim-graph' NodeExpr
void FAnimLangParser::ParseTopLevel(TSharedPtr<FAnimGraphAST> AST)
{
	// :keyword form
	if (Check(EAnimLangTokenType::Keyword))
	{
		FString Key = Current().Value;
		Advance();
		
		if (Key == TEXT("skeleton"))
		{
			if (Check(EAnimLangTokenType::String))
			{
				AST->SkeletonPath = Advance().Value;
			}
			else
			{
				Error(TEXT("Expected string after :skeleton"));
			}
		}
		else if (Key == TEXT("implements"))
		{
			// :implements [ (interface "...") ... ]
			if (!Expect(EAnimLangTokenType::LBracket, TEXT("implements")))
				return;
			while (!IsAtEnd() && !Check(EAnimLangTokenType::RBracket))
			{
				if (Check(EAnimLangTokenType::LParen))
				{
					Advance(); // consume '('
					// expect identifier "interface"
					if (CheckValue(EAnimLangTokenType::Identifier, TEXT("interface")))
					{
						Advance(); // consume "interface"
						if (Check(EAnimLangTokenType::String))
						{
							AST->ImplementedInterfaces.Add(Advance().Value);
						}
						else
						{
							Error(TEXT("Expected string path after 'interface'"));
						}
					}
					else
					{
						Error(FString::Printf(TEXT("Expected 'interface' in :implements block, got '%s'"), *Current().Value));
					}
					if (!Expect(EAnimLangTokenType::RParen, TEXT("interface entry")))
						break;
				}
				else
				{
					Error(FString::Printf(TEXT("Expected '(' in :implements block, got '%s'"), *Current().Value));
					Advance();
				}
			}
			Expect(EAnimLangTokenType::RBracket, TEXT("implements"));
		}
		else if (Key == TEXT("variables"))
		{
			ParseVariables(AST);
		}
		else if (Key == TEXT("anim-graph"))
		{
			AST->RootNode = ParseNodeExpr();
		}
		else
		{
			// Unknown top-level keyword — skip its value
			Error(FString::Printf(TEXT("Unknown top-level keyword :%s"), *Key));
			if (IsValueStart() || IsNodeExprStart())
			{
				ParseValue();  // consume the value to keep going
			}
		}
		return;
	}
	
	// (define ...) form
	if (Check(EAnimLangTokenType::LParen))
	{
		// Peek ahead to see if it's (define ...)
		if (Peek(1).Type == EAnimLangTokenType::Identifier && Peek(1).Value == TEXT("define"))
		{
			ParseDefine(AST);
			return;
		}
		
		// Could be an unnamed node expression at top level? Treat as anim-graph.
		if (!AST->RootNode.IsValid())
		{
			AST->RootNode = ParseNodeExpr();
			return;
		}
	}
	
	// Unexpected token
	Error(FString::Printf(TEXT("Unexpected token at top level: %s(%s)"),
		*FAnimLangToken::TypeToString(Current().Type), *Current().Value));
	Advance();
}

// ParseVariables: '[' VarDef* ']'
void FAnimLangParser::ParseVariables(TSharedPtr<FAnimGraphAST> AST)
{
	if (!Expect(EAnimLangTokenType::LBracket, TEXT("variables")))
	{
		return;
	}
	
	while (!IsAtEnd() && !Check(EAnimLangTokenType::RBracket))
	{
		FVariableDef Var = ParseVarDef();
		AST->Variables.Add(Var);
	}
	
	Expect(EAnimLangTokenType::RBracket, TEXT("variables"));
}

// VarDef: '(' TypeName ':' IDENT Value? ')'
FVariableDef FAnimLangParser::ParseVarDef()
{
	FVariableDef Var;
	
	if (!Expect(EAnimLangTokenType::LParen, TEXT("variable definition")))
	{
		Synchronize();
		return Var;
	}
	
	// Type name (identifier)
	if (Check(EAnimLangTokenType::Identifier))
	{
		FString TypeStr = Advance().Value;
		if (TypeStr == TEXT("float") || TypeStr == TEXT("real") || TypeStr == TEXT("double"))
			Var.Type = EPinType::Float;
		else if (TypeStr == TEXT("int"))
			Var.Type = EPinType::Int;
		else if (TypeStr == TEXT("bool"))
			Var.Type = EPinType::Bool;
		else if (TypeStr == TEXT("vector"))
			Var.Type = EPinType::Vector;
		else if (TypeStr == TEXT("rotator"))
			Var.Type = EPinType::Rotator;
		else if (TypeStr == TEXT("transform"))
			Var.Type = EPinType::Transform;
		else if (TypeStr == TEXT("name"))
			Var.Type = EPinType::Name;
		else
			Var.Type = EPinType::Float;  // default fallback
	}
	else
	{
		Error(TEXT("Expected type name in variable definition"));
	}
	
	// Variable name as keyword (:VarName)
	if (Check(EAnimLangTokenType::Keyword))
	{
		Var.Name = Advance().Value;
	}
	else
	{
		Error(TEXT("Expected :name in variable definition"));
	}
	
	// Optional default value
	if (!Check(EAnimLangTokenType::RParen))
	{
		Var.DefaultValue = ParseValue();
	}
	
	Expect(EAnimLangTokenType::RParen, TEXT("variable definition"));
	return Var;
}

// ParseDefine: '(' 'define' IDENT NodeExpr ')'
void FAnimLangParser::ParseDefine(TSharedPtr<FAnimGraphAST> AST)
{
	Expect(EAnimLangTokenType::LParen, TEXT("define"));
	
	// consume 'define'
	if (CheckValue(EAnimLangTokenType::Identifier, TEXT("define")))
	{
		Advance();
	}
	else
	{
		Error(TEXT("Expected 'define'"));
		return;
	}
	
	FCachedPoseDef Def;
	
	// Name (identifier, could be multi-word with hyphens)
	if (Check(EAnimLangTokenType::Identifier))
	{
		Def.Name = Advance().Value;
		// Convert kebab-case back to spaced: "Post-Layering" → "Post Layering"
		// Actually store as-is; the GetIdentifier() method handles conversion
	}
	else
	{
		Error(TEXT("Expected define name"));
		Synchronize();
		return;
	}
	
	// Body: a node expression
	Def.Body = ParseNodeExpr();
	
	// Closing paren for the define form: (define Name (body))
	Expect(EAnimLangTokenType::RParen, TEXT("define"));
	
	AST->Defines.Add(Def);
}

// NodeExpr ::= '(' NodeType Property* PoseInput* ')'
//            | IDENT   -- bare variable reference (UseCachedPose)
TSharedPtr<FAnimNodeAST> FAnimLangParser::ParseNodeExpr()
{
	// Bare identifier: variable reference
	if (Check(EAnimLangTokenType::Identifier) && !Check(EAnimLangTokenType::LParen))
	{
		TSharedPtr<FAnimNodeAST> Node = MakeShared<FAnimNodeAST>();
		Node->NodeType = Advance().Value;
		return Node;
	}
	
	if (!Check(EAnimLangTokenType::LParen))
	{
		Error(FString::Printf(TEXT("Expected node expression (parenthesized or identifier), got %s(%s)"),
			*FAnimLangToken::TypeToString(Current().Type), *Current().Value));
		return nullptr;
	}
	
	Advance();  // consume '('
	return ParseNodeBody();
}

// ParseNodeBody: NodeType Property* PoseInput* ')'
// Called after the opening '(' has been consumed
TSharedPtr<FAnimNodeAST> FAnimLangParser::ParseNodeBody()
{
	TSharedPtr<FAnimNodeAST> Node = MakeShared<FAnimNodeAST>();
	
	// Node type (identifier)
	if (Check(EAnimLangTokenType::Identifier))
	{
		Node->NodeType = Advance().Value;
	}
	else
	{
		Error(FString::Printf(TEXT("Expected node type identifier, got %s(%s)"),
			*FAnimLangToken::TypeToString(Current().Type), *Current().Value));
		Synchronize();
		if (Check(EAnimLangTokenType::RParen)) Advance();
		return Node;
	}
	
	// Parse properties and children until ')'
	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		// :keyword — could be a property (scalar value) or a named child (node expr)
		if (Check(EAnimLangTokenType::Keyword))
		{
			FString Key = Current().Value;
			Advance();
			
			// Special case: :node-id — store directly in NodeId field, not Properties
			if (Key == TEXT("node-id"))
			{
				if (Check(EAnimLangTokenType::String))
				{
					Node->NodeId = Advance().Value;
				}
				else
				{
					Error(TEXT("Expected string value for :node-id"));
				}
				continue;
			}
			
			// Special case: :transitions [...] — inline array
			if (Key == TEXT("transitions"))
			{
				FString TransValue = ParseTransitionList();
				Node->Properties.Add(Key, TransValue);
				continue;
			}
			
			// Determine: is the next thing a node expression (→ named child) or a value (→ property)?
			if (Check(EAnimLangTokenType::LParen))
			{
				// Could be a node expression like (sequence-player ...)
				// Or could be a value like (ref "...") or (asset "...")
				// Peek to determine:
				if (Peek(1).Type == EAnimLangTokenType::Identifier)
				{
					FString NextIdent = Peek(1).Value;
					if (NextIdent == TEXT("ref") || NextIdent == TEXT("asset") || NextIdent == TEXT("var"))
					{
						// It's a special value form: (ref "..."), (asset "..."), or (var "...")
						FString Value = ParseValue();
						Node->Properties.Add(Key, Value);
					}
					else
					{
						// It's a child node expression
						TSharedPtr<FAnimNodeAST> Child = ParseNodeExpr();
						if (Child.IsValid())
						{
							Node->AddChild(Key, Child);
						}
					}
				}
				else
				{
					// Starts with ( but next is not an identifier — parse as value
					FString Value = ParseValue();
					Node->Properties.Add(Key, Value);
				}
			}
			else if (IsNodeExprStart() && !Check(EAnimLangTokenType::String) && 
					 !Check(EAnimLangTokenType::Integer) && !Check(EAnimLangTokenType::Float) && 
					 !Check(EAnimLangTokenType::Bool))
			{
				// Bare identifier as child node (e.g., :base-pose Post-Layering)
				TSharedPtr<FAnimNodeAST> Child = ParseNodeExpr();
				if (Child.IsValid())
				{
					Node->AddChild(Key, Child);
				}
			}
			else
			{
				// Scalar value
				FString Value = ParseValue();
				Node->Properties.Add(Key, Value);
			}
		}
		else if (Check(EAnimLangTokenType::LParen))
		{
			// Unnamed child node
			TSharedPtr<FAnimNodeAST> Child = ParseNodeExpr();
			if (Child.IsValid())
			{
				Node->AddChild(Child);
			}
		}
		else if (Check(EAnimLangTokenType::Identifier))
		{
			// Could be a bare variable reference as unnamed child
			TSharedPtr<FAnimNodeAST> Child = ParseNodeExpr();
			if (Child.IsValid())
			{
				Node->AddChild(Child);
			}
		}
		else
		{
			// Unexpected token inside node
			Error(FString::Printf(TEXT("Unexpected token in node '%s': %s(%s)"),
				*Node->NodeType, *FAnimLangToken::TypeToString(Current().Type), *Current().Value));
			Advance();
		}
	}
	
	Expect(EAnimLangTokenType::RParen, FString::Printf(TEXT("node '%s'"), *Node->NodeType));
	return Node;
}

// Parse a scalar/composite value
FString FAnimLangParser::ParseValue()
{
	// String literal
	if (Check(EAnimLangTokenType::String))
	{
		FString Val = FString::Printf(TEXT("\"%s\""), *Current().Value);
		Advance();
		return Val;
	}
	
	// Number
	if (Check(EAnimLangTokenType::Integer) || Check(EAnimLangTokenType::Float))
	{
		return Advance().Value;
	}
	
	// Boolean
	if (Check(EAnimLangTokenType::Bool))
	{
		return Advance().Value;
	}
	
	// (ref "...") or (asset "...")
	if (Check(EAnimLangTokenType::LParen))
	{
		if (Peek(1).Type == EAnimLangTokenType::Identifier)
		{
		FString Form = Peek(1).Value;
		if (Form == TEXT("ref") || Form == TEXT("asset") || Form == TEXT("var"))
			{
				Advance();  // (
				FString Keyword = Advance().Value;  // ref or asset
				
				FString Arg;
				if (Check(EAnimLangTokenType::String))
				{
					Arg = FString::Printf(TEXT("\"%s\""), *Current().Value);
					Advance();
				}
				else
				{
					Error(FString::Printf(TEXT("Expected string argument for (%s ...)"), *Keyword));
					Arg = TEXT("\"\"");
				}
				
				Expect(EAnimLangTokenType::RParen, FString::Printf(TEXT("(%s ...)"), *Keyword));
				
				return FString::Printf(TEXT("(%s %s)"), *Keyword, *Arg);
			}
		}
	}
	
	// [...] array
	if (Check(EAnimLangTokenType::LBracket))
	{
		return ParseTransitionList();
	}
	
	// Bare identifier as value
	if (Check(EAnimLangTokenType::Identifier))
	{
		return Advance().Value;
	}
	
	// Keyword as value (shouldn't normally happen)
	if (Check(EAnimLangTokenType::Keyword))
	{
		return FString::Printf(TEXT(":%s"), *Advance().Value);
	}
	
	Error(FString::Printf(TEXT("Expected value, got %s(%s)"),
		*FAnimLangToken::TypeToString(Current().Type), *Current().Value));
	return TEXT("");
}

// Parse [...] transition list or generic array value
// Returns the entire [...] content as a string for round-trip fidelity
FString FAnimLangParser::ParseTransitionList()
{
	if (!Check(EAnimLangTokenType::LBracket))
	{
		Error(TEXT("Expected '[' for transition list"));
		return TEXT("[]");
	}
	
	Advance();  // consume [
	
	FString Result = TEXT("[");
	int32 Depth = 0;
	bool bFirst = true;
	
	while (!IsAtEnd() && !Check(EAnimLangTokenType::RBracket))
	{
		if (!bFirst) Result += TEXT(" ");
		bFirst = false;
		
		if (Check(EAnimLangTokenType::LParen))
		{
			// Parse a transition: (From -> To :key val ...)
			Result += TEXT("(");
			Advance();
			
			bool bFirstInTrans = true;
			while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
			{
				const FAnimLangToken& Tok = Current();
				
				if (Tok.Type == EAnimLangTokenType::Arrow)
				{
					Result += TEXT(" -> ");
					bFirstInTrans = true;  // Next token after -> should not have leading space
				}
				else if (Tok.Type == EAnimLangTokenType::Keyword)
				{
					Result += FString::Printf(TEXT(" :%s"), *Tok.Value);
					bFirstInTrans = false;
				}
				else if (Tok.Type == EAnimLangTokenType::String)
				{
					if (bFirstInTrans)
					{
						Result += EscapeStringForDSL(Tok.Value);
						bFirstInTrans = false;
					}
					else
					{
						Result += TEXT(" ") + EscapeStringForDSL(Tok.Value);
					}
				}
				else if (Tok.Type == EAnimLangTokenType::LParen)
				{
					// Nested expression like (ref "...") or (auto-rule ...)
					int32 InnerDepth = 1;
					Result += TEXT(" (");
					Advance();
					bool bFirstInNested = true;
					while (!IsAtEnd() && InnerDepth > 0)
					{
						const FAnimLangToken& Inner = Current();
						if (Inner.Type == EAnimLangTokenType::LParen) InnerDepth++;
						else if (Inner.Type == EAnimLangTokenType::RParen) InnerDepth--;
						
						if (InnerDepth > 0)
						{
							if (Inner.Type == EAnimLangTokenType::Keyword)
							{
								Result += FString::Printf(TEXT(" :%s"), *Inner.Value);
								bFirstInNested = false;
							}
							else if (Inner.Type == EAnimLangTokenType::String)
							{
								if (bFirstInNested)
								{
									Result += EscapeStringForDSL(Inner.Value);
									bFirstInNested = false;
								}
								else
								{
									Result += TEXT(" ") + EscapeStringForDSL(Inner.Value);
								}
							}
							else
							{
								if (bFirstInNested)
								{
									Result += Inner.Value;
									bFirstInNested = false;
								}
								else
								{
									Result += TEXT(" ") + Inner.Value;
								}
							}
						}
						Advance();
					}
					Result += TEXT(")");
					bFirstInTrans = false;
					continue;  // Already advanced past RParen
				}
				else
				{
					if (bFirstInTrans)
					{
						Result += Tok.Value;
						bFirstInTrans = false;
					}
					else
					{
						Result += TEXT(" ") + Tok.Value;
					}
				}
				
				Advance();
			}
			
			Result += TEXT(")");
			if (Check(EAnimLangTokenType::RParen))
			{
				Advance();
			}
		}
		else
		{
			// Non-parenthesized value in array
			Result += Current().Value;
			Advance();
		}
	}
	
	Result += TEXT("]");
	Expect(EAnimLangTokenType::RBracket, TEXT("transition list"));
	
	return Result;
}

// ========== Helpers ==========

bool FAnimLangParser::IsValueStart() const
{
	switch (Current().Type)
	{
	case EAnimLangTokenType::String:
	case EAnimLangTokenType::Integer:
	case EAnimLangTokenType::Float:
	case EAnimLangTokenType::Bool:
	case EAnimLangTokenType::LParen:
	case EAnimLangTokenType::LBracket:
	case EAnimLangTokenType::Identifier:
		return true;
	default:
		return false;
	}
}

bool FAnimLangParser::IsNodeExprStart() const
{
	return Check(EAnimLangTokenType::LParen) || Check(EAnimLangTokenType::Identifier);
}
