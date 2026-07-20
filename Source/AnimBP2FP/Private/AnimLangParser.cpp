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

static EPinType ParsePinTypeFromText(const FString& TypeText)
{
	const FString Lower = TypeText.ToLower();
	if (Lower == TEXT("pose"))      return EPinType::Pose;
	if (Lower == TEXT("float") || Lower == TEXT("real") || Lower == TEXT("double")) return EPinType::Float;
	if (Lower == TEXT("int"))       return EPinType::Int;
	if (Lower == TEXT("bool"))      return EPinType::Bool;
	if (Lower == TEXT("vector"))    return EPinType::Vector;
	if (Lower == TEXT("rotator"))   return EPinType::Rotator;
	if (Lower == TEXT("transform")) return EPinType::Transform;
	if (Lower == TEXT("name"))      return EPinType::Name;
	if (Lower == TEXT("enum"))      return EPinType::Enum;
	if (Lower == TEXT("object"))    return EPinType::Object;
	if (Lower == TEXT("struct"))    return EPinType::Struct;
	return EPinType::Unknown;
}

static FString UnquoteAnimLangValue(FString Value)
{
	if (Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")) && Value.Len() >= 2)
	{
		Value = Value.Mid(1, Value.Len() - 2);
	}
	return Value;
}

static bool IsAnimPropertyValueForm(const FString& FormName)
{
	return FormName == TEXT("ref")
		|| FormName == TEXT("asset")
		|| FormName == TEXT("var")
		|| FormName == TEXT("bind-var")
		|| FormName == TEXT("bind-path")
		|| FormName == TEXT("subgraph-ref")
		|| FormName == TEXT("unsupported-ref");
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
//            | '(' 'helpers' HelperGraph* ')'
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
		// Peek ahead to see if it's (helpers ...)
		if (Peek(1).Type == EAnimLangTokenType::Identifier && Peek(1).Value == TEXT("helpers"))
		{
			ParseHelpers(AST);
			return;
		}
		if (Peek(1).Type == EAnimLangTokenType::Identifier && Peek(1).Value == TEXT("logic-graphs"))
		{
			ParseLogicGraphs(AST);
			return;
		}
		if (Peek(1).Type == EAnimLangTokenType::Identifier && Peek(1).Value == TEXT("metadata"))
		{
			ParseMetadata(AST);
			return;
		}
		if (Peek(1).Type == EAnimLangTokenType::Identifier && Peek(1).Value == TEXT("dependencies"))
		{
			ParseDependencies(AST);
			return;
		}

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
		Var.Type = ParsePinTypeFromText(Advance().Value);
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
	
	while (!Check(EAnimLangTokenType::RParen))
	{
		if (Check(EAnimLangTokenType::Keyword))
		{
			const FString FieldName = Advance().Value;
			if (FieldName == TEXT("pin-category"))
			{
				Var.PinCategory = UnquoteAnimLangValue(ParseValue());
				continue;
			}
			if (FieldName == TEXT("pin-subcategory"))
			{
				Var.PinSubCategory = UnquoteAnimLangValue(ParseValue());
				continue;
			}
			if (FieldName == TEXT("container"))
			{
				Var.ContainerType = UnquoteAnimLangValue(ParseValue());
				continue;
			}
			if (FieldName == TEXT("reference") || FieldName == TEXT("const")
				|| FieldName == TEXT("weak") || FieldName == TEXT("object-wrapper"))
			{
				const bool bValue = ParseValue().Equals(TEXT("true"), ESearchCase::IgnoreCase);
				if (FieldName == TEXT("reference")) Var.bIsReference = bValue;
				else if (FieldName == TEXT("const")) Var.bIsConst = bValue;
				else if (FieldName == TEXT("weak")) Var.bIsWeakPointer = bValue;
				else Var.bIsUObjectWrapper = bValue;
				continue;
			}
			if (FieldName == TEXT("type-object"))
			{
				FString TypeObjectValue = ParseValue();
				if (TypeObjectValue.StartsWith(TEXT("(asset ")))
				{
					TypeObjectValue.RemoveFromStart(TEXT("(asset "));
					TypeObjectValue.RemoveFromEnd(TEXT(")"));
				}
				if (TypeObjectValue.StartsWith(TEXT("\"")) && TypeObjectValue.EndsWith(TEXT("\"")) && TypeObjectValue.Len() >= 2)
				{
					TypeObjectValue = TypeObjectValue.Mid(1, TypeObjectValue.Len() - 2);
				}
				Var.TypeObjectPath = TypeObjectValue;
				continue;
			}
		}
		
		Var.DefaultValue = ParseValue();
		break;
	}
	
	Expect(EAnimLangTokenType::RParen, TEXT("variable definition"));
	return Var;
}

void FAnimLangParser::ParseHelpers(TSharedPtr<FAnimGraphAST> AST)
{
	Expect(EAnimLangTokenType::LParen, TEXT("helpers"));

	if (CheckValue(EAnimLangTokenType::Identifier, TEXT("helpers")))
	{
		Advance();
	}
	else
	{
		Error(TEXT("Expected 'helpers'"));
		return;
	}

	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		if (Check(EAnimLangTokenType::LParen)
			&& Peek(1).Type == EAnimLangTokenType::Identifier
			&& Peek(1).Value == TEXT("helper-graph"))
		{
			AST->HelperGraphs.Add(ParseHelperGraphDef());
			continue;
		}

		Error(FString::Printf(TEXT("Unexpected token in helpers block: %s(%s)"),
			*FAnimLangToken::TypeToString(Current().Type), *Current().Value));

		if (Check(EAnimLangTokenType::Keyword) || IsValueStart())
		{
			ParseRawExpressionText();
		}
		else
		{
			Advance();
		}
	}

	Expect(EAnimLangTokenType::RParen, TEXT("helpers"));
}

FHelperGraphDef FAnimLangParser::ParseHelperGraphDef()
{
	FHelperGraphDef Helper;

	Expect(EAnimLangTokenType::LParen, TEXT("helper-graph"));
	if (CheckValue(EAnimLangTokenType::Identifier, TEXT("helper-graph")))
	{
		Advance();
	}
	else
	{
		Error(TEXT("Expected 'helper-graph'"));
		return Helper;
	}

	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		if (!Check(EAnimLangTokenType::Keyword))
		{
			Error(FString::Printf(TEXT("Expected helper field keyword, got %s(%s)"),
				*FAnimLangToken::TypeToString(Current().Type), *Current().Value));
			if (Check(EAnimLangTokenType::Keyword) || IsValueStart())
			{
				ParseRawExpressionText();
			}
			else
			{
				Advance();
			}
			continue;
		}

		const FString Key = Advance().Value;
		if (Key == TEXT("id"))
		{
			if (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))
			{
				Helper.Id = Advance().Value;
			}
			else
			{
				Error(TEXT("Expected string or identifier after :id"));
				Helper.Id = ParseRawExpressionText();
			}
		}
		else if (Key == TEXT("graph-name"))
		{
			if (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))
			{
				Helper.GraphName = Advance().Value;
			}
			else
			{
				Error(TEXT("Expected string or identifier after :graph-name"));
				Helper.GraphName = ParseRawExpressionText();
			}
		}
		else if (Key == TEXT("generated-var"))
		{
			if (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))
			{
				Helper.GeneratedVar = Advance().Value;
			}
			else
			{
				Error(TEXT("Expected string or identifier after :generated-var"));
				Helper.GeneratedVar = ParseRawExpressionText();
			}
		}
		else if (Key == TEXT("generated-type"))
		{
			if (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))
			{
				Helper.GeneratedType = ParsePinTypeFromText(Advance().Value);
			}
			else
			{
				Error(TEXT("Expected type name after :generated-type"));
				if (Check(EAnimLangTokenType::Keyword) || IsValueStart())
				{
					ParseRawExpressionText();
				}
			}
		}
		else if (Key == TEXT("update-group"))
		{
			if (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))
			{
				Helper.UpdateGroup = Advance().Value;
			}
			else
			{
				Error(TEXT("Expected string or identifier after :update-group"));
				Helper.UpdateGroup = ParseRawExpressionText();
			}
		}
		else if (Key == TEXT("dsl"))
		{
			if (Check(EAnimLangTokenType::String))
			{
				Helper.DSL = Advance().Value;
			}
			else
			{
				Helper.DSL = ParseRawExpressionText();
			}
		}
		else
		{
			Error(FString::Printf(TEXT("Unknown helper field :%s"), *Key));
			if (Check(EAnimLangTokenType::Keyword) || IsValueStart())
			{
				ParseRawExpressionText();
			}
		}
	}

	Expect(EAnimLangTokenType::RParen, TEXT("helper-graph"));
	return Helper;
}

void FAnimLangParser::ParseLogicGraphs(TSharedPtr<FAnimGraphAST> AST)
{
	AST->bHasLogicGraphsBlock = true;
	Expect(EAnimLangTokenType::LParen, TEXT("logic-graphs"));
	if (!CheckValue(EAnimLangTokenType::Identifier, TEXT("logic-graphs")))
	{
		Error(TEXT("Expected 'logic-graphs'"));
		return;
	}
	Advance();

	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		if (Check(EAnimLangTokenType::LParen)
			&& Peek(1).Type == EAnimLangTokenType::Identifier
			&& Peek(1).Value == TEXT("logic-graph"))
		{
			AST->LogicGraphs.Add(ParseLogicGraphDef());
			continue;
		}
		Error(TEXT("Expected logic-graph entry"));
		Advance();
	}
	Expect(EAnimLangTokenType::RParen, TEXT("logic-graphs"));
}

FLogicGraphDef FAnimLangParser::ParseLogicGraphDef()
{
	FLogicGraphDef Graph;
	Expect(EAnimLangTokenType::LParen, TEXT("logic-graph"));
	if (CheckValue(EAnimLangTokenType::Identifier, TEXT("logic-graph")))
	{
		Advance();
	}
	else
	{
		Error(TEXT("Expected 'logic-graph'"));
		return Graph;
	}

	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		if (!Check(EAnimLangTokenType::Keyword))
		{
			Error(TEXT("Expected logic graph field keyword"));
			Advance();
			continue;
		}
		const FString Key = Advance().Value;
		const FString Value = (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))
			? Advance().Value : ParseRawExpressionText();
		if (Key == TEXT("role")) Graph.Role = Value;
		else if (Key == TEXT("kind")) Graph.Kind = Value;
		else if (Key == TEXT("graph-name")) Graph.GraphName = Value;
		else if (Key == TEXT("schema")) Graph.SchemaClassPath = Value;
		else if (Key == TEXT("dsl")) Graph.DSL = Value;
		else Error(FString::Printf(TEXT("Unknown logic graph field :%s"), *Key));
	}
	Expect(EAnimLangTokenType::RParen, TEXT("logic-graph"));
	return Graph;
}

void FAnimLangParser::ParseMetadata(TSharedPtr<FAnimGraphAST> AST)
{
	Expect(EAnimLangTokenType::LParen, TEXT("metadata"));
	if (!CheckValue(EAnimLangTokenType::Identifier, TEXT("metadata")))
	{
		Error(TEXT("Expected 'metadata'"));
		return;
	}
	Advance();
	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		if (!Check(EAnimLangTokenType::Keyword))
		{
			Error(TEXT("Expected metadata field keyword"));
			Advance();
			continue;
		}
		const FString Key = Advance().Value;
		if (Key == TEXT("root-motion-mode") && (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier)))
		{
			AST->Metadata.RootMotionMode = Advance().Value;
		}
		else
		{
			Error(FString::Printf(TEXT("Unknown or invalid metadata field :%s"), *Key));
			if (IsValueStart()) ParseRawExpressionText();
		}
	}
	Expect(EAnimLangTokenType::RParen, TEXT("metadata"));
}

void FAnimLangParser::ParseDependencies(TSharedPtr<FAnimGraphAST> AST)
{
	Expect(EAnimLangTokenType::LParen, TEXT("dependencies"));
	if (!CheckValue(EAnimLangTokenType::Identifier, TEXT("dependencies")))
	{
		Error(TEXT("Expected 'dependencies'"));
		return;
	}
	Advance();
	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		if (Check(EAnimLangTokenType::LParen) && Peek(1).Value == TEXT("dependency"))
		{
			AST->Dependencies.Add(ParseDependency());
		}
		else
		{
			Error(TEXT("Expected dependency entry"));
			Advance();
		}
	}
	Expect(EAnimLangTokenType::RParen, TEXT("dependencies"));
	AST->Dependencies.Sort([](const FAnimDependency& A, const FAnimDependency& B)
	{
		if (A.ObjectPath != B.ObjectPath) return A.ObjectPath < B.ObjectPath;
		if (A.ClassPath != B.ClassPath) return A.ClassPath < B.ClassPath;
		return A.Role < B.Role;
	});
}

FAnimDependency FAnimLangParser::ParseDependency()
{
	FAnimDependency Dependency;
	Expect(EAnimLangTokenType::LParen, TEXT("dependency"));
	if (!CheckValue(EAnimLangTokenType::Identifier, TEXT("dependency")))
	{
		Error(TEXT("Expected 'dependency'"));
		return Dependency;
	}
	Advance();
	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		if (!Check(EAnimLangTokenType::Keyword))
		{
			Error(TEXT("Expected dependency field keyword"));
			Advance();
			continue;
		}
		const FString Key = Advance().Value;
		if (Key == TEXT("snapshot"))
		{
			Dependency.AssetMetadata = ParseAnimationAssetMetadata();
			continue;
		}
		if (!(Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier)))
		{
			Error(FString::Printf(TEXT("Expected scalar dependency value for :%s"), *Key));
			if (IsValueStart()) ParseRawExpressionText();
			continue;
		}
		const FString Value = Advance().Value;
		if (Key == TEXT("mode")) Dependency.Mode = Value;
		else if (Key == TEXT("object-path")) Dependency.ObjectPath = Value;
		else if (Key == TEXT("class-path")) Dependency.ClassPath = Value;
		else if (Key == TEXT("role")) Dependency.Role = Value;
		else Error(FString::Printf(TEXT("Unknown dependency field :%s"), *Key));
	}
	Expect(EAnimLangTokenType::RParen, TEXT("dependency"));
	return Dependency;
}

FAnimationAssetMetadataSnapshot FAnimLangParser::ParseAnimationAssetMetadata()
{
	FAnimationAssetMetadataSnapshot Snapshot;
	if (!Expect(EAnimLangTokenType::LParen, TEXT("animation-asset-metadata"))) return Snapshot;
	if (!CheckValue(EAnimLangTokenType::Identifier, TEXT("animation-asset-metadata")))
	{
		Error(TEXT("Expected 'animation-asset-metadata'"));
		return Snapshot;
	}
	Advance();
	Snapshot.bHasSnapshot = true;
	auto ReadBool = [this](bool& Out)
	{
		if (Check(EAnimLangTokenType::Bool) || Check(EAnimLangTokenType::Identifier))
		{
			Out = Advance().Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
		}
		else Error(TEXT("Expected boolean metadata value"));
	};
	auto ReadEntryFields = [this](TMap<FString, FString>& Fields)
	{
		while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			if (!Check(EAnimLangTokenType::Keyword)) { Error(TEXT("Expected snapshot entry field")); Advance(); continue; }
			const FString Key = Advance().Value;
			if (Current().IsLiteral() || Check(EAnimLangTokenType::Identifier)) Fields.Add(Key, Advance().Value);
			else { Error(TEXT("Expected snapshot entry scalar")); if (IsValueStart()) ParseRawExpressionText(); }
		}
	};
	while (!IsAtEnd() && !Check(EAnimLangTokenType::RParen))
	{
		if (!Check(EAnimLangTokenType::Keyword)) { Error(TEXT("Expected animation metadata field")); Advance(); continue; }
		const FString Key = Advance().Value;
		if (Key == TEXT("has-root-motion")) ReadBool(Snapshot.bHasRootMotion);
		else if (Key == TEXT("enable-root-motion")) ReadBool(Snapshot.bEnableRootMotion);
		else if (Key == TEXT("force-root-lock")) ReadBool(Snapshot.bForceRootLock);
		else if (Key == TEXT("root-motion-root-lock") && (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))) Snapshot.RootMotionRootLock = Advance().Value;
		else if (Key == TEXT("notifies") || Key == TEXT("sync-markers") || Key == TEXT("montage-sections"))
		{
			const FString EntryType = Key == TEXT("notifies") ? TEXT("notify") : (Key == TEXT("sync-markers") ? TEXT("sync-marker") : TEXT("montage-section"));
			Expect(EAnimLangTokenType::LBracket, *Key);
			while (!IsAtEnd() && !Check(EAnimLangTokenType::RBracket))
			{
				Expect(EAnimLangTokenType::LParen, *EntryType);
				if (CheckValue(EAnimLangTokenType::Identifier, EntryType)) Advance(); else Error(TEXT("Unexpected snapshot entry type"));
				TMap<FString, FString> Fields;
				ReadEntryFields(Fields);
				Expect(EAnimLangTokenType::RParen, *EntryType);
				if (EntryType == TEXT("notify"))
				{
					FAnimNotifySnapshot& N = Snapshot.Notifies.AddDefaulted_GetRef();
					N.ClassPath = Fields.FindRef(TEXT("class-path")); N.Name = Fields.FindRef(TEXT("name")); N.Time = FCString::Atof(*Fields.FindRef(TEXT("time"))); N.Duration = FCString::Atof(*Fields.FindRef(TEXT("duration"))); N.bIsState = Fields.FindRef(TEXT("state")).ToBool();
				}
				else if (EntryType == TEXT("sync-marker"))
				{
					FAnimSyncMarkerSnapshot& M = Snapshot.SyncMarkers.AddDefaulted_GetRef(); M.Name = Fields.FindRef(TEXT("name")); M.Time = FCString::Atof(*Fields.FindRef(TEXT("time")));
				}
				else
				{
					FMontageSectionSnapshot& S = Snapshot.MontageSections.AddDefaulted_GetRef(); S.Name = Fields.FindRef(TEXT("name")); S.StartTime = FCString::Atof(*Fields.FindRef(TEXT("start-time"))); S.NextSectionName = Fields.FindRef(TEXT("next-section"));
				}
			}
			Expect(EAnimLangTokenType::RBracket, *Key);
		}
		else if (Key == TEXT("slot-tracks") || Key == TEXT("unsupported"))
		{
			Expect(EAnimLangTokenType::LBracket, *Key);
			while (!IsAtEnd() && !Check(EAnimLangTokenType::RBracket))
			{
				if (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))
				{
					(Key == TEXT("slot-tracks") ? Snapshot.SlotTrackNames : Snapshot.UnsupportedFields).Add(Advance().Value);
				}
				else Advance();
			}
			Expect(EAnimLangTokenType::RBracket, *Key);
		}
		else { Error(FString::Printf(TEXT("Unknown animation metadata field :%s"), *Key)); if (IsValueStart()) ParseRawExpressionText(); }
	}
	Expect(EAnimLangTokenType::RParen, TEXT("animation-asset-metadata"));
	return Snapshot;
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

			if (Key == TEXT("node-class"))
			{
				if (Check(EAnimLangTokenType::String))
				{
					Node->NodeClassPath = Advance().Value;
				}
				else
				{
					Error(TEXT("Expected string value for :node-class"));
				}
				continue;
			}

			if (Key == TEXT("coverage"))
			{
				if (!Check(EAnimLangTokenType::Identifier))
				{
					Error(TEXT("Expected identifier value for :coverage"));
					continue;
				}
				const FString Coverage = Advance().Value;
				if (Coverage == TEXT("exact")) Node->Coverage = EAnimNodeCoverage::Exact;
				else if (Coverage == TEXT("reflected")) Node->Coverage = EAnimNodeCoverage::Reflected;
				else if (Coverage == TEXT("lossy")) Node->Coverage = EAnimNodeCoverage::Lossy;
				else if (Coverage == TEXT("unsupported")) Node->Coverage = EAnimNodeCoverage::Unsupported;
				else Error(FString::Printf(TEXT("Unknown :coverage value '%s'"), *Coverage));
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
					if (IsAnimPropertyValueForm(NextIdent))
					{
						// It's a special value form such as (ref ...), (asset ...), (bind-var ...), or (subgraph-ref ...)
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
	
	// Special property value forms such as (ref ...), (asset ...), (bind-var ...), or (subgraph-ref ...)
	if (Check(EAnimLangTokenType::LParen) && Peek(1).Type == EAnimLangTokenType::Identifier)
	{
		const FString Form = Peek(1).Value;
		if (IsAnimPropertyValueForm(Form))
		{
			return ParseRawExpressionText();
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

FString FAnimLangParser::ParseRawExpressionText()
{
	if (Check(EAnimLangTokenType::String))
	{
		const FString Result = EscapeStringForDSL(Current().Value);
		Advance();
		return Result;
	}

	if (Check(EAnimLangTokenType::Integer)
		|| Check(EAnimLangTokenType::Float)
		|| Check(EAnimLangTokenType::Bool)
		|| Check(EAnimLangTokenType::Identifier)
		|| Check(EAnimLangTokenType::Arrow))
	{
		const FString Result = Current().Value;
		Advance();
		return Result;
	}

	if (Check(EAnimLangTokenType::Keyword))
	{
		const FString Result = FString::Printf(TEXT(":%s"), *Current().Value);
		Advance();
		return Result;
	}

	if (Check(EAnimLangTokenType::LParen) || Check(EAnimLangTokenType::LBracket))
	{
		const bool bIsList = Check(EAnimLangTokenType::LParen);
		const EAnimLangTokenType ClosingType = bIsList ? EAnimLangTokenType::RParen : EAnimLangTokenType::RBracket;
		const FString OpenText = bIsList ? TEXT("(") : TEXT("[");
		const FString CloseText = bIsList ? TEXT(")") : TEXT("]");

		Advance();

		TArray<FString> Parts;
		while (!IsAtEnd() && !Check(ClosingType))
		{
			Parts.Add(ParseRawExpressionText());
		}

		Expect(ClosingType, TEXT("raw expression"));
		return OpenText + FString::Join(Parts, TEXT(" ")) + CloseText;
	}

	Error(FString::Printf(TEXT("Expected raw expression, got %s(%s)"),
		*FAnimLangToken::TypeToString(Current().Type), *Current().Value));
	return TEXT("");
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
