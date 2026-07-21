// RigLangParser.cpp - RigLang parser built on the shared AnimLisp tokenizer
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "RigLangParser.h"

#include "AnimLangParser.h"
#include "AnimLangTokenizer.h"

namespace
{
class FRigLangParserImpl
{
public:
	FRigLangParserImpl(
		const TArray<FAnimLangToken>& InTokens,
		TArray<FRigLangParseError>& InErrors)
		: Tokens(InTokens)
		, Errors(InErrors)
	{
	}

	TSharedPtr<FRigModuleAST> ParseModule()
	{
		TSharedPtr<FRigModuleAST> Module = MakeShared<FRigModuleAST>();
		bool bSawModuleHeader = false;

		while (!AtEnd())
		{
			if (!Check(EAnimLangTokenType::LParen))
			{
				ErrorAt(Current(), TEXT("Expected a top-level RigLang form"));
				Advance();
				continue;
			}

			const FAnimLangToken Open = Advance();
			if (!Check(EAnimLangTokenType::Identifier))
			{
				ErrorAt(Current(), TEXT("Expected a top-level form name"));
				SkipFormBody();
				continue;
			}

			const FAnimLangToken Head = Advance();
			if (Head.Value == TEXT("rig-module"))
			{
				if (bSawModuleHeader)
				{
					ErrorAt(Head, TEXT("Duplicate rig-module header"));
					SkipFormBody();
				}
				else
				{
					bSawModuleHeader = true;
					ParseModuleHeader(Open, Head, Module->Header);
				}
			}
			else if (Head.Value == TEXT("import-rig"))
			{
				ParseImport(Open, Head, Module->Imports);
			}
			else if (Head.Value == TEXT("rig-hierarchy"))
			{
				ParseHierarchy(Open, Module->Hierarchy);
			}
			else if (Head.Value == TEXT("rig-variables"))
			{
				ParseVariables(Open, Module->Variables);
			}
			else if (Head.Value == TEXT("define-rig-function"))
			{
				ParseFunction(Open, Head, Module->Functions);
			}
			else if (Head.Value == TEXT("define-rig-entry"))
			{
				ParseEntry(Open, Head, Module->Entries);
			}
			else
			{
				ErrorAt(Head, FString::Printf(TEXT("Unsupported top-level form '%s'"), *Head.Value));
				SkipFormBody();
			}
		}

		if (!bSawModuleHeader)
		{
			ErrorAt(Current(), TEXT("Missing rig-module header"));
		}
		ValidateSemanticIdentities(*Module);
		return Errors.IsEmpty() ? Module : nullptr;
	}

private:
	static constexpr int32 MaxNestingDepth = 256;

	const TArray<FAnimLangToken>& Tokens;
	TArray<FRigLangParseError>& Errors;
	int32 Pos = 0;

	const FAnimLangToken& Current() const
	{
		return Tokens[FMath::Clamp(Pos, 0, Tokens.Num() - 1)];
	}

	const FAnimLangToken& Advance()
	{
		const FAnimLangToken& Token = Current();
		if (!AtEnd()) ++Pos;
		return Token;
	}

	bool AtEnd() const { return Current().Type == EAnimLangTokenType::EndOfFile; }
	bool Check(const EAnimLangTokenType Type) const { return Current().Type == Type; }

	void ErrorAt(const FAnimLangToken& Token, const FString& Message)
	{
		FRigLangParseError& Error = Errors.AddDefaulted_GetRef();
		Error.Message = Message;
		Error.Location = Token.Span;
		if (Error.Location.Line == 0)
		{
			Error.Location.Line = Token.Line;
			Error.Location.Column = Token.Column;
			Error.Location.Offset = Token.Offset;
		}
	}

	void ErrorAtLocation(const FAnimLangSourceLoc& Location, const FString& Message)
	{
		FRigLangParseError& Error = Errors.AddDefaulted_GetRef();
		Error.Message = Message;
		Error.Location = Location;
	}

	void ValidateSemanticIdentities(const FRigModuleAST& Module)
	{
		TSet<FString> SeenImportAliases;
		for (const FRigImportAST& Import : Module.Imports)
		{
			const FString& Alias = Import.Import.Alias;
			if (!Alias.IsEmpty() && SeenImportAliases.Contains(Alias))
			{
				ErrorAtLocation(Import.Import.Location, FString::Printf(
					TEXT("Duplicate import alias '%s'"), *Alias));
			}
			else if (!Alias.IsEmpty()) SeenImportAliases.Add(Alias);
		}

		TSet<FString> SeenHierarchyIds;
		TSet<FString> SeenHierarchyNames;
		for (const FRigHierarchyElementAST& Element : Module.Hierarchy)
		{
			if (SeenHierarchyIds.Contains(Element.StableId))
			{
				ErrorAtLocation(Element.Location, FString::Printf(
					TEXT("Duplicate hierarchy stable ID '%s'"), *Element.StableId));
			}
			else SeenHierarchyIds.Add(Element.StableId);

			if (!Element.Name.IsEmpty() && SeenHierarchyNames.Contains(Element.Name))
			{
				ErrorAtLocation(Element.Location, FString::Printf(
					TEXT("Duplicate hierarchy name '%s'"), *Element.Name));
			}
			else if (!Element.Name.IsEmpty()) SeenHierarchyNames.Add(Element.Name);
		}

		TSet<FString> SeenVariableIds;
		TSet<FString> SeenVariableNames;
		for (const FRigVariableAST& Variable : Module.Variables)
		{
			if (SeenVariableIds.Contains(Variable.StableId))
			{
				ErrorAtLocation(Variable.Location, FString::Printf(
					TEXT("Duplicate variable stable ID '%s'"), *Variable.StableId));
			}
			else SeenVariableIds.Add(Variable.StableId);

			if (!Variable.Name.IsEmpty() && SeenVariableNames.Contains(Variable.Name))
			{
				ErrorAtLocation(Variable.Location, FString::Printf(
					TEXT("Duplicate variable name '%s'"), *Variable.Name));
			}
			else if (!Variable.Name.IsEmpty()) SeenVariableNames.Add(Variable.Name);
		}

		TSet<FString> SeenFunctionIds;
		for (const FRigFunctionAST& Function : Module.Functions)
		{
			if (SeenFunctionIds.Contains(Function.StableId))
			{
				ErrorAtLocation(Function.Location, FString::Printf(
					TEXT("Duplicate function stable ID '%s'"), *Function.StableId));
			}
			else SeenFunctionIds.Add(Function.StableId);
			ValidateGraphNodeIds(Function.Graph);
		}

		TSet<FString> SeenEntryIds;
		for (const FRigEntryAST& Entry : Module.Entries)
		{
			if (SeenEntryIds.Contains(Entry.StableId))
			{
				ErrorAtLocation(Entry.Location, FString::Printf(
					TEXT("Duplicate entry stable ID '%s'"), *Entry.StableId));
			}
			else SeenEntryIds.Add(Entry.StableId);
			ValidateGraphNodeIds(Entry.Graph);
		}

		struct FRigSymbolDeclaration
		{
			FString Name;
			FString Kind;
			FAnimLangSourceLoc Location;
		};
		TArray<FRigSymbolDeclaration> Symbols;
		for (const FRigFunctionAST& Function : Module.Functions)
		{
			Symbols.Add({Function.Name, TEXT("function"), Function.Location});
		}
		for (const FRigEntryAST& Entry : Module.Entries)
		{
			Symbols.Add({Entry.Name, TEXT("entry"), Entry.Location});
		}
		Symbols.Sort([](const FRigSymbolDeclaration& A, const FRigSymbolDeclaration& B)
		{
			return A.Location.Offset < B.Location.Offset;
		});

		TMap<FString, FString> SymbolKinds;
		for (const FRigSymbolDeclaration& Symbol : Symbols)
		{
			if (Symbol.Name.IsEmpty()) continue;
			if (const FString* ExistingKind = SymbolKinds.Find(Symbol.Name))
			{
				const FString Message = *ExistingKind == Symbol.Kind
					? FString::Printf(TEXT("Duplicate %s name '%s'"), *Symbol.Kind, *Symbol.Name)
					: FString::Printf(TEXT("Duplicate rig symbol name '%s'"), *Symbol.Name);
				ErrorAtLocation(Symbol.Location, Message);
			}
			else
			{
				SymbolKinds.Add(Symbol.Name, Symbol.Kind);
			}
		}
	}

	void ValidateGraphNodeIds(const FRigGraphAST& Graph)
	{
		TSet<FString> SeenNodeIds;
		for (const FRigNodeAST& Node : Graph.Nodes)
		{
			if (SeenNodeIds.Contains(Node.StableId))
			{
				ErrorAtLocation(Node.Location, FString::Printf(
					TEXT("Duplicate node stable ID '%s'"), *Node.StableId));
			}
			else SeenNodeIds.Add(Node.StableId);
		}
	}

	void SkipFormBody()
	{
		int32 Depth = 1;
		while (!AtEnd() && Depth > 0)
		{
			if (Check(EAnimLangTokenType::LParen)) ++Depth;
			else if (Check(EAnimLangTokenType::RParen)) --Depth;
			Advance();
		}
	}

	void SkipValue()
	{
		if (!Check(EAnimLangTokenType::LParen) && !Check(EAnimLangTokenType::LBracket))
		{
			if (!AtEnd()) Advance();
			return;
		}

		const EAnimLangTokenType OpenType = Current().Type;
		const EAnimLangTokenType CloseType = OpenType == EAnimLangTokenType::LParen
			? EAnimLangTokenType::RParen
			: EAnimLangTokenType::RBracket;
		int32 Depth = 0;
		do
		{
			if (Check(OpenType)) ++Depth;
			else if (Check(CloseType)) --Depth;
			Advance();
		}
		while (!AtEnd() && Depth > 0);
	}

	bool ConsumeClose(const FAnimLangToken& Open, const FString& FormName)
	{
		if (Check(EAnimLangTokenType::RParen))
		{
			Advance();
			return true;
		}
		if (AtEnd())
		{
			ErrorAt(Open, FString::Printf(TEXT("Unbalanced form '%s'"), *FormName));
		}
		else
		{
			ErrorAt(Current(), FString::Printf(TEXT("Expected ')' to close %s"), *FormName));
			Advance();
		}
		return false;
	}

	bool ReadString(FString& OutValue, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::String))
		{
			ErrorAt(Current(), FString::Printf(TEXT("Expected string value for :%s"), *Field));
			SkipValue();
			return false;
		}
		OutValue = Advance().Value;
		return true;
	}

	bool ReadIdentifier(FString& OutValue, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::Identifier))
		{
			ErrorAt(Current(), FString::Printf(TEXT("Expected identifier value for :%s"), *Field));
			SkipValue();
			return false;
		}
		OutValue = Advance().Value;
		return true;
	}

	bool ReadName(FString& OutValue, const FString& Context)
	{
		if (Check(EAnimLangTokenType::String) || Check(EAnimLangTokenType::Identifier))
		{
			OutValue = Advance().Value;
			return true;
		}
		ErrorAt(Current(), FString::Printf(TEXT("Expected name for %s"), *Context));
		SkipValue();
		return false;
	}

	bool ReadBool(bool& OutValue, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::Bool))
		{
			ErrorAt(Current(), FString::Printf(TEXT("Expected boolean value for :%s"), *Field));
			SkipValue();
			return false;
		}
		OutValue = Advance().Value == TEXT("true");
		return true;
	}

	bool ReadInt(int32& OutValue, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::Integer))
		{
			ErrorAt(Current(), FString::Printf(TEXT("Expected integer value for :%s"), *Field));
			SkipValue();
			return false;
		}
		OutValue = FCString::Atoi(*Advance().Value);
		return true;
	}

	static FString QuoteRaw(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
		Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
		Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
		Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
		return TEXT("\"") + Escaped + TEXT("\"");
	}

	void SkipCompositeBody(const EAnimLangTokenType OpenType)
	{
		TArray<EAnimLangTokenType> ClosingStack;
		ClosingStack.Add(OpenType == EAnimLangTokenType::LParen
			? EAnimLangTokenType::RParen
			: EAnimLangTokenType::RBracket);
		while (!AtEnd() && !ClosingStack.IsEmpty())
		{
			if (Check(EAnimLangTokenType::LParen))
			{
				ClosingStack.Add(EAnimLangTokenType::RParen);
				Advance();
			}
			else if (Check(EAnimLangTokenType::LBracket))
			{
				ClosingStack.Add(EAnimLangTokenType::RBracket);
				Advance();
			}
			else if (Current().Type == ClosingStack.Last())
			{
				ClosingStack.Pop(EAllowShrinking::No);
				Advance();
			}
			else
			{
				Advance();
			}
		}
	}

	FString ParseRawValue(const int32 Depth = 0)
	{
		if (Check(EAnimLangTokenType::String)) return QuoteRaw(Advance().Value);
		if (Check(EAnimLangTokenType::Keyword)) return TEXT(":") + Advance().Value;
		if (Check(EAnimLangTokenType::Identifier)
			|| Check(EAnimLangTokenType::Integer)
			|| Check(EAnimLangTokenType::Float)
			|| Check(EAnimLangTokenType::Bool)
			|| Check(EAnimLangTokenType::Arrow))
		{
			return Advance().Value;
		}

		if (Check(EAnimLangTokenType::LParen) || Check(EAnimLangTokenType::LBracket))
		{
			const FAnimLangToken Open = Advance();
			const bool bParen = Open.Type == EAnimLangTokenType::LParen;
			const int32 CompositeDepth = Depth + 1;
			if (CompositeDepth > MaxNestingDepth)
			{
				ErrorAt(Open, FString::Printf(
					TEXT("Maximum RigLang nesting depth (%d) exceeded"),
					MaxNestingDepth));
				SkipCompositeBody(Open.Type);
				return bParen ? TEXT("()") : TEXT("[]");
			}
			const EAnimLangTokenType CloseType = bParen ? EAnimLangTokenType::RParen : EAnimLangTokenType::RBracket;
			TArray<FString> Parts;
			while (!AtEnd() && !Check(CloseType))
			{
				Parts.Add(ParseRawValue(CompositeDepth));
			}
			if (!Check(CloseType))
			{
				ErrorAt(Open, TEXT("Unbalanced property value"));
			}
			else
			{
				Advance();
			}
			return (bParen ? TEXT("(") : TEXT("["))
				+ FString::Join(Parts, TEXT(" "))
				+ (bParen ? TEXT(")") : TEXT("]"));
		}

		ErrorAt(Current(), TEXT("Expected property value"));
		SkipValue();
		return FString();
	}

	void ReadUnknownProperty(const FString& Key, TMap<FString, FString>& Properties)
	{
		Properties.Add(Key, ParseRawValue());
	}

	bool NextKeyword(TSet<FString>& SeenProperties, FString& OutKey)
	{
		if (!Check(EAnimLangTokenType::Keyword)) return false;
		const FAnimLangToken Keyword = Advance();
		OutKey = Keyword.Value;
		if (SeenProperties.Contains(OutKey))
		{
			ErrorAt(Keyword, FString::Printf(TEXT("Duplicate property :%s"), *OutKey));
		}
		else
		{
			SeenProperties.Add(OutKey);
		}
		return true;
	}

	void ParseModuleHeader(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head,
		FRigModuleHeaderAST& Header)
	{
		Header.Location = Head.Span;
		FString AssetPath;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected rig-module property"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("asset")) ReadString(AssetPath, Key);
			else if (Key == TEXT("class")) ReadString(Header.AssetClassPath, Key);
			else if (Key == TEXT("version")) ReadInt(Header.Version, Key);
			else if (Key == TEXT("content-hash")) ReadString(Header.ContentHash, Key);
			else ReadUnknownProperty(Key, Header.Properties);
		}
		ConsumeClose(Open, TEXT("rig-module"));
		if (AssetPath.IsEmpty()) ErrorAt(Head, TEXT("rig-module requires :asset"));
		if (Header.AssetClassPath.IsEmpty()) ErrorAt(Head, TEXT("rig-module requires :class"));
		if (Header.Version == INDEX_NONE) ErrorAt(Head, TEXT("rig-module requires :version"));
		if (Header.ContentHash.IsEmpty()) ErrorAt(Head, TEXT("rig-module requires :content-hash"));
		Header.ModuleId = FAnimLispModuleId::FromAssetPath(AssetPath, EAnimLispModuleKind::Rig);
	}

	void ParseImport(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head,
		TArray<FRigImportAST>& Imports)
	{
		FRigImportAST Import;
		Import.Import.Location = Head.Span;
		FString AssetPath;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected import-rig property"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("asset")) ReadString(AssetPath, Key);
			else if (Key == TEXT("alias")) ReadIdentifier(Import.Import.Alias, Key);
			else if (Key == TEXT("content-hash")) ReadString(Import.Import.ExpectedHash, Key);
			else ReadUnknownProperty(Key, Import.Properties);
		}
		ConsumeClose(Open, TEXT("import-rig"));
		if (AssetPath.IsEmpty()) ErrorAt(Head, TEXT("import-rig requires :asset"));
		if (Import.Import.Alias.IsEmpty()) ErrorAt(Head, TEXT("import-rig requires :alias"));
		Import.Import.Target = FAnimLispModuleId::FromAssetPath(AssetPath, EAnimLispModuleKind::Rig);
		Imports.Add(MoveTemp(Import));
	}

	void ParseHierarchy(const FAnimLangToken& Open, TArray<FRigHierarchyElementAST>& Hierarchy)
	{
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			if (!Check(EAnimLangTokenType::LParen))
			{
				ErrorAt(Current(), TEXT("Expected hierarchy element form"));
				SkipValue();
				continue;
			}
			const FAnimLangToken ElementOpen = Advance();
			if (!Check(EAnimLangTokenType::Identifier))
			{
				ErrorAt(Current(), TEXT("Expected hierarchy element kind"));
				SkipFormBody();
				continue;
			}
			const FAnimLangToken Head = Advance();
			if (Head.Value != TEXT("bone") && Head.Value != TEXT("control"))
			{
				ErrorAt(Head, FString::Printf(TEXT("Unsupported hierarchy element '%s'"), *Head.Value));
				SkipFormBody();
				continue;
			}

			FRigHierarchyElementAST Element;
			Element.Kind = Head.Value == TEXT("bone") ? ERigHierarchyElementKind::Bone : ERigHierarchyElementKind::Control;
			Element.Location = Head.Span;
			TSet<FString> SeenProperties;
			while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
			{
				FString Key;
				if (!NextKeyword(SeenProperties, Key))
				{
					ErrorAt(Current(), TEXT("Expected hierarchy element property"));
					SkipValue();
					continue;
				}
				if (Key == TEXT("id")) ReadString(Element.StableId, Key);
				else if (Key == TEXT("name")) ReadString(Element.Name, Key);
				else if (Key == TEXT("parent")) ReadString(Element.ParentName, Key);
				else ReadUnknownProperty(Key, Element.Properties);
			}
			ConsumeClose(ElementOpen, Head.Value);
			if (Element.StableId.IsEmpty()) ErrorAt(Head, FString::Printf(TEXT("%s requires :id"), *Head.Value));
			if (Element.Name.IsEmpty()) ErrorAt(Head, FString::Printf(TEXT("%s requires :name"), *Head.Value));
			Hierarchy.Add(MoveTemp(Element));
		}
		ConsumeClose(Open, TEXT("rig-hierarchy"));
	}

	static bool ParseAccess(const FString& Value, ERigVariableAccess& OutAccess)
	{
		if (Value == TEXT("public-input")) OutAccess = ERigVariableAccess::PublicInput;
		else if (Value == TEXT("public-output")) OutAccess = ERigVariableAccess::PublicOutput;
		else if (Value == TEXT("internal")) OutAccess = ERigVariableAccess::Internal;
		else return false;
		return true;
	}

	void ParseVariables(const FAnimLangToken& Open, TArray<FRigVariableAST>& Variables)
	{
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			if (!Check(EAnimLangTokenType::LParen))
			{
				ErrorAt(Current(), TEXT("Expected variable form"));
				SkipValue();
				continue;
			}
			const FAnimLangToken VariableOpen = Advance();
			if (!Check(EAnimLangTokenType::Identifier))
			{
				ErrorAt(Current(), TEXT("Expected variable form name"));
				SkipFormBody();
				continue;
			}
			const FAnimLangToken Head = Advance();
			if (Head.Value != TEXT("variable"))
			{
				ErrorAt(Head, FString::Printf(TEXT("Unsupported rig-variables form '%s'"), *Head.Value));
				SkipFormBody();
				continue;
			}

			FRigVariableAST Variable;
			Variable.Location = Head.Span;
			TSet<FString> SeenProperties;
			while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
			{
				FString Key;
				if (!NextKeyword(SeenProperties, Key))
				{
					ErrorAt(Current(), TEXT("Expected variable property"));
					SkipValue();
					continue;
				}
				if (Key == TEXT("id")) ReadString(Variable.StableId, Key);
				else if (Key == TEXT("name")) ReadString(Variable.Name, Key);
				else if (Key == TEXT("access"))
				{
					FString Value;
					if (ReadIdentifier(Value, Key) && !ParseAccess(Value, Variable.Access))
					{
						ErrorAt(Tokens[Pos - 1], FString::Printf(TEXT("Unsupported variable access '%s'"), *Value));
					}
				}
				else if (Key == TEXT("cpp-type")) ReadString(Variable.Type.CPPType, Key);
				else if (Key == TEXT("cpp-type-object")) ReadString(Variable.Type.CPPTypeObject, Key);
				else if (Key == TEXT("container-type")) ReadString(Variable.Type.ContainerType, Key);
				else if (Key == TEXT("default")) ReadString(Variable.DefaultValue, Key);
				else if (Key == TEXT("execute-context")) ReadBool(Variable.bExecuteContext, Key);
				else ReadUnknownProperty(Key, Variable.Properties);
			}
			ConsumeClose(VariableOpen, TEXT("variable"));
			if (Variable.StableId.IsEmpty()) ErrorAt(Head, TEXT("variable requires :id"));
			if (Variable.Name.IsEmpty()) ErrorAt(Head, TEXT("variable requires :name"));
			if (Variable.Type.CPPType.IsEmpty()) ErrorAt(Head, TEXT("variable requires :cpp-type"));
			Variables.Add(MoveTemp(Variable));
		}
		ConsumeClose(Open, TEXT("rig-variables"));
	}

	static bool ParseDirection(const FString& Value, ERigPinDirection& OutDirection)
	{
		if (Value == TEXT("input")) OutDirection = ERigPinDirection::Input;
		else if (Value == TEXT("output")) OutDirection = ERigPinDirection::Output;
		else if (Value == TEXT("io")) OutDirection = ERigPinDirection::IO;
		else if (Value == TEXT("hidden")) OutDirection = ERigPinDirection::Hidden;
		else return false;
		return true;
	}

	static bool ParseCoverage(const FString& Value, ERigNodeCoverage& OutCoverage)
	{
		if (Value == TEXT("exact")) OutCoverage = ERigNodeCoverage::Exact;
		else if (Value == TEXT("reflected")) OutCoverage = ERigNodeCoverage::Reflected;
		else if (Value == TEXT("lossy")) OutCoverage = ERigNodeCoverage::Lossy;
		else if (Value == TEXT("unsupported")) OutCoverage = ERigNodeCoverage::Unsupported;
		else return false;
		return true;
	}

	FRigPinAST ParsePin(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head,
		TSet<FString>& SeenPinPaths,
		const int32 Depth)
	{
		FRigPinAST Pin;
		Pin.Location = Head.Span;
		if (Depth > MaxNestingDepth)
		{
			ErrorAt(Head, FString::Printf(
				TEXT("Maximum RigLang nesting depth (%d) exceeded"),
				MaxNestingDepth));
			SkipFormBody();
			return Pin;
		}
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			if (Check(EAnimLangTokenType::LParen))
			{
				const FAnimLangToken SubOpen = Advance();
				if (!Check(EAnimLangTokenType::Identifier))
				{
					ErrorAt(Current(), TEXT("Expected pin sub-form name"));
					SkipFormBody();
					continue;
				}
				const FAnimLangToken SubHead = Advance();
				if (SubHead.Value != TEXT("pin"))
				{
					ErrorAt(SubHead, FString::Printf(TEXT("Unsupported connected pin form '%s'"), *SubHead.Value));
					SkipFormBody();
					continue;
				}
				Pin.SubPins.Add(ParsePin(SubOpen, SubHead, SeenPinPaths, Depth + 1));
				continue;
			}

			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected pin property or subpin"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("path")) ReadString(Pin.Path, Key);
			else if (Key == TEXT("direction"))
			{
				FString Value;
				if (ReadIdentifier(Value, Key) && !ParseDirection(Value, Pin.Direction))
				{
					ErrorAt(Tokens[Pos - 1], FString::Printf(TEXT("Unsupported pin direction '%s'"), *Value));
				}
			}
			else if (Key == TEXT("cpp-type")) ReadString(Pin.Type.CPPType, Key);
			else if (Key == TEXT("cpp-type-object")) ReadString(Pin.Type.CPPTypeObject, Key);
			else if (Key == TEXT("container-type")) ReadString(Pin.Type.ContainerType, Key);
			else if (Key == TEXT("default")) ReadString(Pin.DefaultValue, Key);
			else if (Key == TEXT("execute-context")) ReadBool(Pin.bExecuteContext, Key);
			else ReadUnknownProperty(Key, Pin.Properties);
		}
		ConsumeClose(Open, TEXT("pin"));
		if (Pin.Path.IsEmpty()) ErrorAt(Head, TEXT("pin requires :path"));
		else if (SeenPinPaths.Contains(Pin.Path))
		{
			ErrorAt(Head, FString::Printf(TEXT("Duplicate pin path '%s'"), *Pin.Path));
		}
		else SeenPinPaths.Add(Pin.Path);
		if (Pin.Type.CPPType.IsEmpty()) ErrorAt(Head, TEXT("pin requires :cpp-type"));
		return Pin;
	}

	FRigNodeAST ParseNode(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head)
	{
		FRigNodeAST Node;
		Node.Kind = Head.Value == TEXT("rig-unit") ? ERigNodeKind::Unit : ERigNodeKind::Call;
		Node.Location = Head.Span;
		TSet<FString> SeenPinPaths;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			if (Check(EAnimLangTokenType::LParen))
			{
				const FAnimLangToken PinOpen = Advance();
				if (!Check(EAnimLangTokenType::Identifier))
				{
					ErrorAt(Current(), TEXT("Expected connected node form name"));
					SkipFormBody();
					continue;
				}
				const FAnimLangToken PinHead = Advance();
				if (PinHead.Value != TEXT("pin"))
				{
					ErrorAt(PinHead, FString::Printf(TEXT("Unsupported connected node semantic form '%s'"), *PinHead.Value));
					SkipFormBody();
					continue;
				}
				Node.Pins.Add(ParsePin(PinOpen, PinHead, SeenPinPaths, 1));
				continue;
			}

			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected node property or pin"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("id")) ReadString(Node.StableId, Key);
			else if (Key == TEXT("guid")) ReadString(Node.Guid, Key);
			else if (Key == TEXT("class")) ReadString(Node.ClassPath, Key);
			else if (Key == TEXT("method")) ReadString(Node.MethodName, Key);
			else if (Key == TEXT("event")) ReadString(Node.EventName, Key);
			else if (Key == TEXT("function")) ReadString(Node.FunctionName, Key);
			else if (Key == TEXT("injected")) ReadBool(Node.bInjected, Key);
			else if (Key == TEXT("coverage"))
			{
				FString Value;
				if (ReadIdentifier(Value, Key) && !ParseCoverage(Value, Node.Coverage))
				{
					ErrorAt(Tokens[Pos - 1], FString::Printf(TEXT("Unsupported node coverage '%s'"), *Value));
				}
			}
			else ReadUnknownProperty(Key, Node.Properties);
		}
		ConsumeClose(Open, Head.Value);
		if (Node.StableId.IsEmpty()) ErrorAt(Head, FString::Printf(TEXT("%s requires :id"), *Head.Value));
		else if (Node.StableId.Contains(TEXT(".")))
		{
			ErrorAt(Head, FString::Printf(
				TEXT("Node stable ID '%s' must not contain '.'"), *Node.StableId));
		}
		if (Node.Guid.IsEmpty()) ErrorAt(Head, FString::Printf(TEXT("%s requires :guid"), *Head.Value));
		if (Node.Kind == ERigNodeKind::Unit && Node.ClassPath.IsEmpty()) ErrorAt(Head, TEXT("rig-unit requires :class"));
		if (Node.Kind == ERigNodeKind::Call && Node.FunctionName.IsEmpty()) ErrorAt(Head, TEXT("rig-call requires :function"));
		return Node;
	}

	FRigLinkAST ParseLink(const FAnimLangToken& Open, const FAnimLangToken& Head)
	{
		FRigLinkAST Link;
		Link.Location = Head.Span;
		FString SourceEndpoint;
		FString TargetEndpoint;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected rig-link property"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("from")) ReadString(SourceEndpoint, Key);
			else if (Key == TEXT("to")) ReadString(TargetEndpoint, Key);
			else ReadUnknownProperty(Key, Link.Properties);
		}
		ConsumeClose(Open, TEXT("rig-link"));
		if (SourceEndpoint.IsEmpty()) ErrorAt(Head, TEXT("rig-link requires :from"));
		else SplitLinkEndpoint(SourceEndpoint, TEXT("from"), Head, Link.SourceNodeId, Link.SourcePinPath);
		if (TargetEndpoint.IsEmpty()) ErrorAt(Head, TEXT("rig-link requires :to"));
		else SplitLinkEndpoint(TargetEndpoint, TEXT("to"), Head, Link.TargetNodeId, Link.TargetPinPath);
		return Link;
	}

	void SplitLinkEndpoint(
		const FString& Endpoint,
		const FString& Field,
		const FAnimLangToken& Head,
		FString& OutNodeId,
		FString& OutPinPath)
	{
		int32 SeparatorIndex = INDEX_NONE;
		if (!Endpoint.FindChar(TEXT('.'), SeparatorIndex)
			|| SeparatorIndex <= 0
			|| SeparatorIndex >= Endpoint.Len() - 1)
		{
			ErrorAt(Head, FString::Printf(
				TEXT("rig-link :%s endpoint '%s' must be '<node-id>.<pin-path>'"),
				*Field,
				*Endpoint));
			return;
		}
		OutNodeId = Endpoint.Left(SeparatorIndex);
		OutPinPath = Endpoint.Mid(SeparatorIndex + 1);
	}

	void ParseGraphMember(FRigGraphAST& Graph)
	{
		const FAnimLangToken Open = Advance();
		if (!Check(EAnimLangTokenType::Identifier))
		{
			ErrorAt(Current(), TEXT("Expected graph form name"));
			SkipFormBody();
			return;
		}
		const FAnimLangToken Head = Advance();
		if (Head.Value == TEXT("rig-unit") || Head.Value == TEXT("rig-call"))
		{
			Graph.Nodes.Add(ParseNode(Open, Head));
		}
		else if (Head.Value == TEXT("rig-link"))
		{
			Graph.Links.Add(ParseLink(Open, Head));
		}
		else
		{
			ErrorAt(Head, FString::Printf(TEXT("Unsupported connected semantic form '%s'"), *Head.Value));
			SkipFormBody();
		}
	}

	void ParseFunction(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head,
		TArray<FRigFunctionAST>& Functions)
	{
		FRigFunctionAST Function;
		Function.Location = Head.Span;
		ReadName(Function.Name, TEXT("define-rig-function"));
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			if (Check(EAnimLangTokenType::LParen))
			{
				ParseGraphMember(Function.Graph);
				continue;
			}
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected function property or graph form"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("id")) ReadString(Function.StableId, Key);
			else if (Key == TEXT("visibility")) ReadIdentifier(Function.Visibility, Key);
			else if (Key == TEXT("return-cpp-type")) ReadString(Function.ReturnCPPType, Key);
			else ReadUnknownProperty(Key, Function.Properties);
		}
		ConsumeClose(Open, TEXT("define-rig-function"));
		if (Function.Name.IsEmpty()) ErrorAt(Head, TEXT("define-rig-function requires a name"));
		if (Function.StableId.IsEmpty()) ErrorAt(Head, TEXT("define-rig-function requires :id"));
		if (Function.Visibility.IsEmpty()) ErrorAt(Head, TEXT("define-rig-function requires :visibility"));
		Functions.Add(MoveTemp(Function));
	}

	void ParseEntry(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head,
		TArray<FRigEntryAST>& Entries)
	{
		FRigEntryAST Entry;
		Entry.Location = Head.Span;
		ReadName(Entry.Name, TEXT("define-rig-entry"));
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			if (Check(EAnimLangTokenType::LParen))
			{
				ParseGraphMember(Entry.Graph);
				continue;
			}
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected entry property or graph form"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("id")) ReadString(Entry.StableId, Key);
			else if (Key == TEXT("event")) ReadString(Entry.EventName, Key);
			else ReadUnknownProperty(Key, Entry.Properties);
		}
		ConsumeClose(Open, TEXT("define-rig-entry"));
		if (Entry.Name.IsEmpty()) ErrorAt(Head, TEXT("define-rig-entry requires a name"));
		if (Entry.StableId.IsEmpty()) ErrorAt(Head, TEXT("define-rig-entry requires :id"));
		if (Entry.EventName.IsEmpty()) ErrorAt(Head, TEXT("define-rig-entry requires :event"));
		Entries.Add(MoveTemp(Entry));
	}
};
}

TSharedPtr<FRigModuleAST> FRigLangParser::Parse(
	const FString& Source,
	const FString& SourceFile,
	TArray<FRigLangParseError>& OutErrors)
{
	OutErrors.Reset();
	TArray<FAnimLangParseError> TokenErrors;
	const TArray<FAnimLangToken> Tokens = FAnimLangTokenizer::Tokenize(Source, SourceFile, &TokenErrors);
	for (const FAnimLangParseError& TokenError : TokenErrors)
	{
		FRigLangParseError& Error = OutErrors.AddDefaulted_GetRef();
		Error.Message = TokenError.Message;
		Error.Location.SourceFile = SourceFile;
		Error.Location.Line = TokenError.Line;
		Error.Location.Column = TokenError.Column;
	}
	if (!OutErrors.IsEmpty())
	{
		return nullptr;
	}

	FRigLangParserImpl Parser(Tokens, OutErrors);
	return Parser.ParseModule();
}
