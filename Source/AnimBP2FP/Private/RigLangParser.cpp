// RigLangParser.cpp - RigLang parser built on the shared AnimLisp tokenizer
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "RigLangParser.h"
#include "RigLangExporter.h"
#include "AnimBP2FPVersionCompat.h"

#include "AnimLangParser.h"
#include "AnimLangTokenizer.h"
#if ENGINE_MAJOR_VERSION < 5
#include "Rigs/RigControlHierarchy.h"
#else
#include "Rigs/RigHierarchyElements.h"
#endif

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
			else if (Head.Value == TEXT("define-rig-graph"))
			{
				ParseGraph(Open, Head, Module->Graphs);
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

		auto HasInlineGraphContent = [](const FRigGraphAST& Graph)
		{
			return Graph.LocalVariables.Num() != 0 || Graph.Nodes.Num() != 0 || Graph.Links.Num() != 0;
		};
		const bool bHasDeclaredGraphInventory = Module->Graphs.Num() != 0;
		if (bHasDeclaredGraphInventory)
		{
			for (const FRigFunctionAST& Function : Module->Functions)
			{
				if (HasInlineGraphContent(Function.Graph))
				{
					ErrorAtLocation(Function.Location, FString::Printf(
						TEXT("Function '%s' has legacy inline graph content alongside module graph inventory"),
						*Function.Name));
				}
			}
			for (const FRigEntryAST& Entry : Module->Entries)
			{
				if (HasInlineGraphContent(Entry.Graph))
				{
					ErrorAtLocation(Entry.Location, FString::Printf(
						TEXT("Entry '%s' has legacy inline graph content alongside module graph inventory"),
						*Entry.Name));
				}
			}
		}
		else
		{
			const bool bNeedsMigration = Module->Functions.ContainsByPredicate(
				[&HasInlineGraphContent](const FRigFunctionAST& Function)
				{
					return HasInlineGraphContent(Function.Graph);
				}) || Module->Entries.ContainsByPredicate(
				[&HasInlineGraphContent](const FRigEntryAST& Entry)
				{
					return HasInlineGraphContent(Entry.Graph);
				});
			const FString LegacyLibraryId = TEXT("legacy-function-library");
			if (bNeedsMigration && Module->Functions.Num() != 0)
			{
				FRigGraphAST& Library = Module->Graphs.AddDefaulted_GetRef();
				Library.StableId = LegacyLibraryId;
				Library.Role = TEXT("function-library");
				Library.Location = Module->Header.Location;
			}
			for (FRigFunctionAST& Function : Module->Functions)
			{
				if (!bNeedsMigration) break;
				const bool bHadInlineContent = HasInlineGraphContent(Function.Graph);
				FRigGraphAST Graph = MoveTemp(Function.Graph);
				Graph.StableId = TEXT("legacy-function-") + Function.StableId;
				Graph.Role = TEXT("function");
				Graph.ParentStableId = LegacyLibraryId;
				Graph.Location = Function.Location;
				Function.GraphStableId = Graph.StableId;
				Module->Graphs.Add(MoveTemp(Graph));
				if (bHadInlineContent) WarningAtLocation(Function.Location, FString::Printf(
					TEXT("legacy-inline-graph-migrated: function '%s' migrated to graph '%s'"),
					*Function.Name, *Function.GraphStableId));
			}
			for (FRigEntryAST& Entry : Module->Entries)
			{
				if (!bNeedsMigration) break;
				const bool bHadInlineContent = HasInlineGraphContent(Entry.Graph);
				FRigGraphAST Graph = MoveTemp(Entry.Graph);
				Graph.StableId = TEXT("legacy-entry-") + Entry.StableId;
				Graph.Role = TEXT("root");
				Graph.Location = Entry.Location;
				Entry.GraphStableId = Graph.StableId;
				Module->Graphs.Add(MoveTemp(Graph));
				if (bHadInlineContent) WarningAtLocation(Entry.Location, FString::Printf(
					TEXT("legacy-inline-graph-migrated: entry '%s' migrated to graph '%s'"),
					*Entry.Name, *Entry.GraphStableId));
			}
		}
		for (FRigGraphAST& Graph : Module->Graphs)
		{
			FGuid ParsedGuid;
			if (Graph.EditorGuid.Len() == 36
				&& FGuid::ParseExact(Graph.EditorGuid, EGuidFormats::DigitsWithHyphens, ParsedGuid)
				&& ParsedGuid.IsValid())
			{
				Graph.EditorGuid = ParsedGuid.ToString(EGuidFormats::DigitsWithHyphens);
			}
			else
			{
				const FString LegacyGuid = Graph.EditorGuid.IsEmpty() ? FString(TEXT("missing")) : Graph.EditorGuid;
				Graph.EditorGuid = FRigLangExporter::ComputeDeterministicEditorGuid(
					Module->Header.ModuleId.AssetPath, Graph.StableId);
				WarningAtLocation(Graph.Location, FString::Printf(
					TEXT("legacy-editor-guid-normalized: graph '%s' editor GUID '%s' normalized to '%s'"),
					*Graph.StableId, *LegacyGuid, *Graph.EditorGuid));
			}
		}
		auto NormalizeCallGraph = [this, &Module](FRigGraphAST& Graph)
		{
			for (FRigNodeAST& Node : Graph.Nodes)
			{
				if (Node.Kind != ERigNodeKind::Call) continue;
				if (!Node.FunctionIdentifier.IsComplete())
				{
					WarningAtLocation(Node.Location, FString::Printf(
						TEXT("legacy-rig-call-identity: call '%s' uses only short symbol '%s'"),
						*Node.StableId, *Node.FunctionName));
					continue;
				}
				if (Node.FunctionName.Contains(TEXT("/")) || Node.FunctionName.Contains(TEXT(".")))
				{
					continue;
				}
				TArray<const FRigFunctionAST*> LocalMatches;
				for (const FRigFunctionAST& Function : Module->Functions)
					if (Function.FunctionIdentifier == Node.FunctionIdentifier) LocalMatches.Add(&Function);
				if (LocalMatches.Num() == 1)
				{
					Node.FunctionName = LocalMatches[0]->Name;
					continue;
				}
				const FRigImportAST* MatchingImport = nullptr;
				for (const FRigImportAST& Import : Module->Imports)
				{
					if (!RigFunctionHostMatchesModule(
						Node.FunctionIdentifier.HostObject, Import.Import.Target.AssetPath)) continue;
					if (MatchingImport != nullptr)
					{
						MatchingImport = nullptr;
						break;
					}
					MatchingImport = &Import;
				}
				if (MatchingImport)
				{
					Node.FunctionName = MatchingImport->Import.Alias + TEXT("/")
						+ RigFunctionSymbolFromLibraryNodePath(Node.FunctionIdentifier.LibraryNodePath);
				}
			}
		};
		for (FRigGraphAST& Graph : Module->Graphs) NormalizeCallGraph(Graph);
		for (FRigFunctionAST& Function : Module->Functions) NormalizeCallGraph(Function.Graph);
		for (FRigEntryAST& Entry : Module->Entries) NormalizeCallGraph(Entry.Graph);
		ValidateSemanticIdentities(*Module);
		return Errors.ContainsByPredicate([](const FRigLangParseError& Error)
		{
			return !Error.bWarning;
		}) ? nullptr : Module;
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

	void WarningAtLocation(const FAnimLangSourceLoc& Location, const FString& Message)
	{
		FRigLangParseError& Warning = Errors.AddDefaulted_GetRef();
		Warning.Message = Message;
		Warning.Location = Location;
		Warning.bWarning = true;
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

		TSet<FString> SeenGraphIds;
		TSet<FString> SeenGraphEditorGuids;
		TMap<FString, const FRigGraphAST*> GraphsById;
		for (const FRigGraphAST& Graph : Module.Graphs)
		{
			if (SeenGraphIds.Contains(Graph.StableId))
			{
				ErrorAtLocation(Graph.Location, FString::Printf(
					TEXT("Duplicate graph stable ID '%s'"), *Graph.StableId));
			}
			else
			{
				SeenGraphIds.Add(Graph.StableId);
				GraphsById.Add(Graph.StableId, &Graph);
			}
			if (SeenGraphEditorGuids.Contains(Graph.EditorGuid))
			{
				ErrorAtLocation(Graph.Location, FString::Printf(
					TEXT("Duplicate graph editor GUID '%s'"), *Graph.EditorGuid));
			}
			else SeenGraphEditorGuids.Add(Graph.EditorGuid);
			ValidateGraphNodeIds(Graph);
		}
		auto EffectiveGraphRole = [&GraphsById](const FRigGraphAST& Graph)
		{
			if (Graph.Role != TEXT("contained")) return Graph.Role;
			const FRigGraphAST* Parent = GraphsById.FindRef(Graph.ParentStableId);
			return Parent && Parent->Role == TEXT("function-library") ? FString(TEXT("function")) : FString(TEXT("node-contained"));
		};
		for (const FRigGraphAST& Graph : Module.Graphs)
		{
			const FString Role = EffectiveGraphRole(Graph);
			const bool bHasParentRole = Role == TEXT("function") || Role == TEXT("node-contained");
			if (bHasParentRole && Graph.ParentStableId.IsEmpty())
			{
				ErrorAtLocation(Graph.Location, FString::Printf(
					TEXT("Graph '%s' with role '%s' requires :parent-id"), *Graph.StableId, *Role));
			}
			else if (!bHasParentRole && !Graph.ParentStableId.IsEmpty())
			{
				ErrorAtLocation(Graph.Location, FString::Printf(
					TEXT("Graph '%s' with role '%s' cannot have :parent-id"), *Graph.StableId, *Role));
			}
			else if (bHasParentRole && !GraphsById.Contains(Graph.ParentStableId))
			{
				ErrorAtLocation(Graph.Location, FString::Printf(
					TEXT("Graph '%s' has missing parent graph '%s'"), *Graph.StableId, *Graph.ParentStableId));
			}
			if (Role != TEXT("root") && Role != TEXT("function-library")
				&& Role != TEXT("function") && Role != TEXT("node-contained"))
			{
				ErrorAtLocation(Graph.Location, FString::Printf(
					TEXT("Graph '%s' has invalid role '%s'"), *Graph.StableId, *Graph.Role));
			}
			if (Role == TEXT("function") && GraphsById.Contains(Graph.ParentStableId)
				&& EffectiveGraphRole(*GraphsById.FindRef(Graph.ParentStableId)) != TEXT("function-library"))
			{
				ErrorAtLocation(Graph.Location, FString::Printf(
					TEXT("Function graph '%s' must have a function-library parent"), *Graph.StableId));
			}
			if (Role == TEXT("node-contained") && GraphsById.Contains(Graph.ParentStableId)
				&& EffectiveGraphRole(*GraphsById.FindRef(Graph.ParentStableId)) == TEXT("function-library"))
			{
				ErrorAtLocation(Graph.Location, FString::Printf(
					TEXT("Node-contained graph '%s' cannot have a function-library parent"), *Graph.StableId));
			}
		}

		TMap<FString, uint8> GraphVisitState;
		TArray<FString> GraphVisitPath;
		TFunction<void(const FString&)> VisitGraphParent = [&](const FString& GraphId)
		{
			if (GraphVisitState.FindRef(GraphId) == 2) return;
			if (GraphVisitState.FindRef(GraphId) == 1)
			{
				const int32 Start = GraphVisitPath.Find(GraphId);
				TArray<FString> Cycle;
				for (int32 Index = Start == INDEX_NONE ? 0 : Start; Index < GraphVisitPath.Num(); ++Index) Cycle.Add(GraphVisitPath[Index]);
				Cycle.Add(GraphId);
				const FRigGraphAST* Graph = GraphsById.FindRef(GraphId);
				ErrorAtLocation(Graph ? Graph->Location : FAnimLangSourceLoc(),
					TEXT("Graph parent cycle: ") + FString::Join(Cycle, TEXT(" -> ")));
				return;
			}
			GraphVisitState.Add(GraphId, 1);
			GraphVisitPath.Add(GraphId);
			if (const FRigGraphAST* Graph = GraphsById.FindRef(GraphId))
				if (GraphsById.Contains(Graph->ParentStableId)) VisitGraphParent(Graph->ParentStableId);
			GraphVisitPath.Pop(ANIMBP2FP_NO_SHRINKING);
			GraphVisitState.Add(GraphId, 2);
		};
		for (const FRigGraphAST& Graph : Module.Graphs) VisitGraphParent(Graph.StableId);

		TSet<FString> SeenFunctionIds;
		TMap<FString, FAnimLangSourceLoc> SeenFunctionIdentifiers;
		for (const FRigFunctionAST& Function : Module.Functions)
		{
			if (SeenFunctionIds.Contains(Function.StableId))
			{
				ErrorAtLocation(Function.Location, FString::Printf(
					TEXT("Duplicate function stable ID '%s'"), *Function.StableId));
			}
			else SeenFunctionIds.Add(Function.StableId);
			if (Function.FunctionIdentifier.IsComplete())
			{
				const FString Identifier = Function.FunctionIdentifier.ToStableId();
				if (const FAnimLangSourceLoc* FirstLocation = SeenFunctionIdentifiers.Find(Identifier))
				{
					ErrorAtLocation(Function.Location, FString::Printf(
						TEXT("Duplicate typed function identifier; first declared at %s"),
						*FirstLocation->ToString()));
				}
				else SeenFunctionIdentifiers.Add(Identifier, Function.Location);
			}
			if (Module.Graphs.Num() != 0)
			{
				if (Function.GraphStableId.IsEmpty())
				{
					ErrorAtLocation(Function.Location, FString::Printf(
						TEXT("Rig function '%s' requires :graph-id"), *Function.Name));
				}
				else if (const FRigGraphAST* const* Graph = GraphsById.Find(Function.GraphStableId))
				{
					if (EffectiveGraphRole(**Graph) != TEXT("function")) ErrorAtLocation(Function.Location, FString::Printf(
						TEXT("Rig function '%s' must reference a function graph"), *Function.Name));
				}
				else ErrorAtLocation(Function.Location, FString::Printf(
					TEXT("Rig function '%s' references missing graph '%s'"), *Function.Name, *Function.GraphStableId));
			}
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
			if (Module.Graphs.Num() != 0)
			{
				if (Entry.GraphStableId.IsEmpty())
				{
					ErrorAtLocation(Entry.Location, FString::Printf(
						TEXT("Rig entry '%s' requires :graph-id"), *Entry.Name));
				}
				else if (const FRigGraphAST* const* Graph = GraphsById.Find(Entry.GraphStableId))
				{
					if (EffectiveGraphRole(**Graph) != TEXT("root")) ErrorAtLocation(Entry.Location, FString::Printf(
						TEXT("Rig entry '%s' must reference a root graph"), *Entry.Name));
				}
				else ErrorAtLocation(Entry.Location, FString::Printf(
					TEXT("Rig entry '%s' references missing graph '%s'"), *Entry.Name, *Entry.GraphStableId));
			}
			ValidateGraphNodeIds(Entry.Graph);
		}

		if (Module.Graphs.Num() != 0)
		{
			TMap<FString, int32> FunctionOwners;
			TMap<FString, int32> EntryOwners;
			for (const FRigFunctionAST& Function : Module.Functions) ++FunctionOwners.FindOrAdd(Function.GraphStableId);
			for (const FRigEntryAST& Entry : Module.Entries) ++EntryOwners.FindOrAdd(Entry.GraphStableId);
			for (const FRigGraphAST& ParentGraph : Module.Graphs)
			{
				for (const FRigNodeAST& Node : ParentGraph.Nodes)
				{
					if (Node.ContainedGraphStableId.IsEmpty()) continue;
					const FRigGraphAST* TargetGraph = GraphsById.FindRef(Node.ContainedGraphStableId);
					if (!TargetGraph)
					{
						ErrorAtLocation(Node.Location, FString::Printf(
							TEXT("Node '%s' references missing contained graph '%s'"), *Node.StableId, *Node.ContainedGraphStableId));
					}
					else if (TargetGraph->ParentStableId != ParentGraph.StableId)
					{
						ErrorAtLocation(Node.Location, FString::Printf(
							TEXT("Node '%s' contained graph '%s' must name '%s' as its parent"),
							*Node.StableId, *Node.ContainedGraphStableId, *ParentGraph.StableId));
					}
				}
			}
			for (const FRigGraphAST& Graph : Module.Graphs)
			{
				const FString Role = EffectiveGraphRole(Graph);
				if (Role == TEXT("function") && FunctionOwners.FindRef(Graph.StableId) != 1)
				{
					ErrorAtLocation(Graph.Location, FString::Printf(
						TEXT("Function graph '%s' must have exactly one function owner"), *Graph.StableId));
				}
				if (Role == TEXT("node-contained"))
				{
					int32 NodeOwnerCount = 0;
					if (const FRigGraphAST* Parent = GraphsById.FindRef(Graph.ParentStableId))
						for (const FRigNodeAST& Node : Parent->Nodes)
							if (Node.ContainedGraphStableId == Graph.StableId) ++NodeOwnerCount;
					if (NodeOwnerCount != 1)
					{
						ErrorAtLocation(Graph.Location, FString::Printf(
							TEXT("Node-contained graph '%s' must have exactly one owning node in parent graph '%s'"),
							*Graph.StableId, *Graph.ParentStableId));
					}
				}
				if (Role == TEXT("root") && EntryOwners.FindRef(Graph.StableId) == 0)
				{
					ErrorAtLocation(Graph.Location, FString::Printf(
						TEXT("Root graph '%s' must have at least one entry owner"), *Graph.StableId));
				}
			}
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
		TSet<FGuid> SeenLocalGuids;
		TSet<FName> SeenLocalNames;
		for (const FRigGraphVariableAST& Variable : Graph.LocalVariables)
		{
			FGuid ParsedGuid;
			if (Variable.Guid.Len() == 36
				&& FGuid::ParseExact(Variable.Guid, EGuidFormats::DigitsWithHyphens, ParsedGuid))
			{
				if (SeenLocalGuids.Contains(ParsedGuid))
					ErrorAtLocation(Variable.Location, FString::Printf(TEXT("Duplicate local variable GUID '%s'"), *Variable.Guid));
				else SeenLocalGuids.Add(ParsedGuid);
			}
			const FName ParsedName(*Variable.Name);
			if (!ParsedName.IsNone())
			{
				if (SeenLocalNames.Contains(ParsedName))
					ErrorAtLocation(Variable.Location, FString::Printf(TEXT("Duplicate local variable name '%s'"), *Variable.Name));
				else SeenLocalNames.Add(ParsedName);
			}
		}
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

	bool ReadUInt32(uint32& OutValue, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::Integer))
		{
			ErrorAt(Current(), FString::Printf(TEXT("Expected unsigned integer value for :%s"), *Field));
			SkipValue();
			return false;
		}
		const FAnimLangToken Token = Advance();
		if (Token.Value.StartsWith(TEXT("-")) || !LexTryParseString(OutValue, *Token.Value))
		{
			ErrorAt(Token, FString::Printf(TEXT("Unsigned integer value for :%s is out of range"), *Field));
			return false;
		}
		return true;
	}

	bool ReadNumber(double& OutValue, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::Integer) && !Check(EAnimLangTokenType::Float))
		{
			ErrorAt(Current(), FString::Printf(TEXT("Expected numeric value for :%s"), *Field));
			SkipValue();
			return false;
		}
		const FAnimLangToken Token = Advance();
		if (!LexTryParseString(OutValue, *Token.Value))
		{
			ErrorAt(Token, FString::Printf(TEXT("Invalid numeric value for :%s"), *Field));
			return false;
		}
		return true;
	}

	bool ReadNumberList(TArray<double>& OutValues, const int32 RequiredCount, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::LParen))
		{
			ErrorAt(Current(), FString::Printf(TEXT(":%s requires %d numeric components"), *Field, RequiredCount));
			SkipValue();
			return false;
		}
		const FAnimLangToken Open = Advance();
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			double Value = 0.0;
			if (!ReadNumber(Value, Field)) break;
			OutValues.Add(Value);
		}
		ConsumeClose(Open, Field);
		if (RequiredCount >= 0 && OutValues.Num() != RequiredCount)
		{
			ErrorAt(Open, FString::Printf(TEXT(":%s requires %d numeric components"), *Field, RequiredCount));
			return false;
		}
		return true;
	}

	bool ReadVector(FVector& OutValue, const FString& Field)
	{
		TArray<double> Values;
		if (!ReadNumberList(Values, 3, Field)) return false;
		OutValue = FVector(Values[0], Values[1], Values[2]);
		return true;
	}

	bool ReadBoolList(TArray<bool>& OutValues, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::LParen)) { ErrorAt(Current(), FString::Printf(TEXT(":%s requires a list"), *Field)); SkipValue(); return false; }
		const FAnimLangToken Open=Advance(); while(!AtEnd()&&!Check(EAnimLangTokenType::RParen)){bool V=false;if(!ReadBool(V,Field))break;OutValues.Add(V);} ConsumeClose(Open,Field); return true;
	}
	bool ReadIntegerList(TArray<int64>& OutValues, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::LParen)) { ErrorAt(Current(), FString::Printf(TEXT(":%s requires a list"), *Field)); SkipValue(); return false; }
		const FAnimLangToken Open=Advance(); while(!AtEnd()&&!Check(EAnimLangTokenType::RParen)){if(!Check(EAnimLangTokenType::Integer)){ErrorAt(Current(),FString::Printf(TEXT("Expected integer value for :%s"),*Field));SkipValue();break;}int64 V=0;LexTryParseString(V,*Advance().Value);OutValues.Add(V);} ConsumeClose(Open,Field); return true;
	}
	bool ReadStringList(TArray<FString>& OutValues, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::LParen)) { ErrorAt(Current(), FString::Printf(TEXT(":%s requires a list"), *Field)); SkipValue(); return false; }
		const FAnimLangToken Open=Advance(); while(!AtEnd()&&!Check(EAnimLangTokenType::RParen)){FString V;if(!ReadString(V,Field))break;OutValues.Add(MoveTemp(V));} ConsumeClose(Open,Field); return true;
	}
	bool ReadTupleList(TArray<TArray<double>>& OutValues, const int32 Arity, const FString& Field)
	{
		if (!Check(EAnimLangTokenType::LParen)) { ErrorAt(Current(), FString::Printf(TEXT(":%s requires a list"), *Field)); SkipValue(); return false; }
		const FAnimLangToken Open=Advance(); while(!AtEnd()&&!Check(EAnimLangTokenType::RParen)){TArray<double> V;if(!ReadNumberList(V,Arity,Field))break;OutValues.Add(MoveTemp(V));} ConsumeClose(Open,Field); return true;
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
		while (!AtEnd() && ClosingStack.Num() != 0)
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
				ClosingStack.Pop(ANIMBP2FP_NO_SHRINKING);
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

	bool MigrateLegacyControlSettings(const FAnimLangToken& Head, FRigHierarchyElementAST& Element)
	{
#if ENGINE_MAJOR_VERSION < 5
		ErrorAt(Head, TEXT("[UNSUPPORTED:UE4TypedRigSnapshot] typed Control Rig settings require Unreal Engine 5"));
		return false;
#else
		const FString* LegacyControlType = Element.Properties.Find(TEXT("control-type"));
		const FString* LegacySettings = Element.Properties.Find(TEXT("settings"));
		if (!LegacyControlType && !LegacySettings) return false;
		if (!LegacyControlType)
		{
			ErrorAt(Head, TEXT("legacy control settings migration requires :control-type"));
			return false;
		}

		const int64 ControlTypeValue = StaticEnum<ERigControlType>()->GetValueByNameString(*LegacyControlType);
		if (ControlTypeValue == INDEX_NONE)
		{
			ErrorAt(Head, FString::Printf(TEXT("legacy control has invalid :control-type '%s'"), **LegacyControlType));
			return false;
		}

		FRigControlSettings Settings;
		Settings.ControlType = static_cast<ERigControlType>(ControlTypeValue);
		bool bMappedShape = false;
		bool bMappedLimits = false;
		if (LegacySettings)
		{
			TArray<FAnimLangToken> LegacyTokens;
			TArray<FAnimLangLexError> LegacyLexErrors;
			if (FAnimLangTokenizer::Tokenize(*LegacySettings, LegacyTokens, LegacyLexErrors))
			{
				for (int32 Index = 0; Index + 1 < LegacyTokens.Num(); ++Index)
				{
					const FAnimLangToken& Token = LegacyTokens[Index];
					if (Token.Type != EAnimLangTokenType::Keyword) continue;
					if (Token.Value == TEXT("shape")
						&& (LegacyTokens[Index + 1].Type == EAnimLangTokenType::String
							|| LegacyTokens[Index + 1].Type == EAnimLangTokenType::Identifier))
					{
						Settings.ShapeName = FName(*LegacyTokens[Index + 1].Value);
						bMappedShape = true;
					}
					else if (Token.Value == TEXT("limits")
						&& LegacyTokens[Index + 1].Type == EAnimLangTokenType::LBracket)
					{
						TArray<FRigControlLimitEnabled> Limits;
						int32 LimitIndex = Index + 2;
						for (; LimitIndex < LegacyTokens.Num()
							&& LegacyTokens[LimitIndex].Type == EAnimLangTokenType::Bool; ++LimitIndex)
						{
							Limits.Add(FRigControlLimitEnabled(LegacyTokens[LimitIndex].Value == TEXT("true")));
						}
						if (LimitIndex < LegacyTokens.Num()
							&& LegacyTokens[LimitIndex].Type == EAnimLangTokenType::RBracket)
						{
							Settings.LimitEnabled = MoveTemp(Limits);
							bMappedLimits = true;
						}
					}
				}
			}
		}

		FRigHierarchyStateAST& State = Element.States.AddDefaulted_GetRef();
		State.Kind = ERigHierarchyStateKind::ControlSettings;
		State.Role = TEXT("initial");
		State.Type = StaticEnum<ERigControlType>()->GetNameStringByValue(ControlTypeValue);
		State.Location = Element.Location;
		FRigControlSettings::StaticStruct()->ExportText(
			State.SerializedValue, &Settings, &Settings, nullptr, PPF_None, nullptr);
		Element.Properties.Remove(TEXT("control-type"));
		Element.Properties.Remove(TEXT("settings"));
		WarningAtLocation(Element.Location, FString::Printf(
			TEXT("legacy-control-settings-lossy-migrated: control '%s' promoted to typed settings; migration is not exact (shape=%s, limits=%s, unspecified fields use engine defaults)"),
			*Element.Name, bMappedShape ? TEXT("mapped") : TEXT("defaulted"), bMappedLimits ? TEXT("mapped") : TEXT("defaulted")));
		return true;
#endif
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
			if (Head.Value != TEXT("bone") && Head.Value != TEXT("control")
				&& Head.Value != TEXT("null") && Head.Value != TEXT("curve"))
			{
				ErrorAt(Head, FString::Printf(TEXT("Unsupported hierarchy element '%s'"), *Head.Value));
				SkipFormBody();
				continue;
			}

			FRigHierarchyElementAST Element;
			if (Head.Value == TEXT("bone")) Element.Kind = ERigHierarchyElementKind::Bone;
			else if (Head.Value == TEXT("control")) Element.Kind = ERigHierarchyElementKind::Control;
			else if (Head.Value == TEXT("null")) Element.Kind = ERigHierarchyElementKind::Null;
			else Element.Kind = ERigHierarchyElementKind::Curve;
			Element.Location = Head.Span;
			TSet<FString> SeenProperties;
			while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
			{
				if (Check(EAnimLangTokenType::LParen))
				{
					const FAnimLangToken ChildOpen = Advance();
					if (!Check(EAnimLangTokenType::Identifier))
					{
						ErrorAt(Current(), TEXT("Expected typed hierarchy form name"));
						SkipFormBody();
						continue;
					}
					const FAnimLangToken ChildHead = Advance();
					if (ChildHead.Value == TEXT("rig-parent")) ParseHierarchyParent(ChildOpen, ChildHead, Element);
					else if (ChildHead.Value == TEXT("rig-transform")) ParseHierarchyTransform(ChildOpen, ChildHead, Element);
					else if (ChildHead.Value == TEXT("rig-state")) ParseHierarchyState(ChildOpen, ChildHead, Element);
					else if (ChildHead.Value == TEXT("rig-metadata")) ParseHierarchyMetadata(ChildOpen, ChildHead, Element);
					else
					{
						ErrorAt(ChildHead, FString::Printf(TEXT("Unsupported typed hierarchy form '%s'"), *ChildHead.Value));
						SkipFormBody();
					}
					continue;
				}
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
			if (Element.Kind == ERigHierarchyElementKind::Control)
			{
				if (!Element.States.ContainsByPredicate([](const FRigHierarchyStateAST& State)
					{ return State.Kind == ERigHierarchyStateKind::ControlSettings; }))
				{
					MigrateLegacyControlSettings(Head, Element);
				}
				const FRigHierarchyStateAST* SettingsState = nullptr;
				int32 SettingsCount = 0;
				for (const FRigHierarchyStateAST& State : Element.States)
				{
					if (State.Kind == ERigHierarchyStateKind::ControlSettings)
					{
						SettingsState = &State;
						++SettingsCount;
					}
				}
				if (SettingsCount != 1)
				{
					ErrorAt(Head, TEXT("control requires exactly one control-settings state"));
				}
				if (SettingsState)
				{
					for (const FRigHierarchyStateAST& State : Element.States)
					{
						if (State.Kind == ERigHierarchyStateKind::ControlValue && State.Type != SettingsState->Type)
						{
							ErrorAtLocation(State.Location, FString::Printf(
								TEXT("control-value type '%s' does not match control-settings type '%s'"),
								*State.Type, *SettingsState->Type));
						}
					}
				}

				const FRigHierarchyStateAST* PreferredEulerState = nullptr;
				for (const FRigHierarchyStateAST& State : Element.States)
				{
					if (State.Kind != ERigHierarchyStateKind::PreferredEuler) continue;
					if (PreferredEulerState && State.Type != PreferredEulerState->Type)
					{
						ErrorAtLocation(State.Location, FString::Printf(
							TEXT("preferred-euler type '%s' does not match '%s'"),
							*State.Type, *PreferredEulerState->Type));
					}
					else PreferredEulerState = &State;
				}
			}
			Hierarchy.Add(MoveTemp(Element));
		}
		ConsumeClose(Open, TEXT("rig-hierarchy"));
	}

	void ParseHierarchyParent(const FAnimLangToken& Open, const FAnimLangToken& Head, FRigHierarchyElementAST& Element)
	{
		FRigHierarchyParentAST Parent;
		Parent.Location = Head.Span;
		TSet<FString> Seen;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key;
			if (!NextKeyword(Seen, Key)) { ErrorAt(Current(), TEXT("Expected rig-parent property")); SkipValue(); continue; }
			if (Key == TEXT("id")) ReadString(Parent.StableId, Key);
			else if (Key == TEXT("label")) ReadString(Parent.Label, Key);
			else if (Key == TEXT("current-location")) ReadNumber(Parent.CurrentWeight.Location, Key);
			else if (Key == TEXT("current-rotation")) ReadNumber(Parent.CurrentWeight.Rotation, Key);
			else if (Key == TEXT("current-scale")) ReadNumber(Parent.CurrentWeight.Scale, Key);
			else if (Key == TEXT("initial-location")) ReadNumber(Parent.InitialWeight.Location, Key);
			else if (Key == TEXT("initial-rotation")) ReadNumber(Parent.InitialWeight.Rotation, Key);
			else if (Key == TEXT("initial-scale")) ReadNumber(Parent.InitialWeight.Scale, Key);
			else { ErrorAt(Current(), FString::Printf(TEXT("Unsupported rig-parent property :%s"), *Key)); SkipValue(); }
		}
		ConsumeClose(Open, TEXT("rig-parent"));
		for (const TCHAR* Required : { TEXT("id"), TEXT("label"), TEXT("current-location"), TEXT("current-rotation"), TEXT("current-scale"), TEXT("initial-location"), TEXT("initial-rotation"), TEXT("initial-scale") })
			if (!Seen.Contains(Required)) ErrorAt(Head, FString::Printf(TEXT("rig-parent requires :%s"), Required));
		Element.Parents.Add(MoveTemp(Parent));
	}

	static bool ParseTransformRole(const FString& Value, ERigHierarchyTransformRole& Out)
	{
		static const TMap<FString, ERigHierarchyTransformRole> Roles = {
			{TEXT("initial-local"), ERigHierarchyTransformRole::InitialLocal}, {TEXT("initial-global"), ERigHierarchyTransformRole::InitialGlobal},
			{TEXT("current-local"), ERigHierarchyTransformRole::CurrentLocal}, {TEXT("current-global"), ERigHierarchyTransformRole::CurrentGlobal},
			{TEXT("pose-initial-local"), ERigHierarchyTransformRole::PoseInitialLocal}, {TEXT("pose-initial-global"), ERigHierarchyTransformRole::PoseInitialGlobal},
			{TEXT("pose-current-local"), ERigHierarchyTransformRole::PoseCurrentLocal}, {TEXT("pose-current-global"), ERigHierarchyTransformRole::PoseCurrentGlobal},
			{TEXT("offset-initial-local"), ERigHierarchyTransformRole::OffsetInitialLocal}, {TEXT("offset-initial-global"), ERigHierarchyTransformRole::OffsetInitialGlobal},
			{TEXT("offset-current-local"), ERigHierarchyTransformRole::OffsetCurrentLocal}, {TEXT("offset-current-global"), ERigHierarchyTransformRole::OffsetCurrentGlobal},
			{TEXT("shape-initial-local"), ERigHierarchyTransformRole::ShapeInitialLocal}, {TEXT("shape-initial-global"), ERigHierarchyTransformRole::ShapeInitialGlobal},
			{TEXT("shape-current-local"), ERigHierarchyTransformRole::ShapeCurrentLocal}, {TEXT("shape-current-global"), ERigHierarchyTransformRole::ShapeCurrentGlobal}
		};
		if (const ERigHierarchyTransformRole* Role = Roles.Find(Value)) { Out = *Role; return true; }
		return false;
	}

	void ParseHierarchyTransform(const FAnimLangToken& Open, const FAnimLangToken& Head, FRigHierarchyElementAST& Element)
	{
		FRigHierarchyTransformAST Transform; Transform.Location = Head.Span;
		FString Role; FVector Translation = FVector::ZeroVector, Scale = FVector::OneVector; FQuat Rotation = FQuat::Identity;
		TSet<FString> Seen;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key; if (!NextKeyword(Seen, Key)) { ErrorAt(Current(), TEXT("Expected rig-transform property")); SkipValue(); continue; }
			if (Key == TEXT("role")) ReadIdentifier(Role, Key);
			else if (Key == TEXT("translation")) ReadVector(Translation, Key);
			else if (Key == TEXT("scale")) ReadVector(Scale, Key);
			else if (Key == TEXT("rotation")) { TArray<double> V; if (ReadNumberList(V, 4, Key)) Rotation = FQuat(V[0], V[1], V[2], V[3]); }
			else { ErrorAt(Current(), FString::Printf(TEXT("Unsupported rig-transform property :%s"), *Key)); SkipValue(); }
		}
		ConsumeClose(Open, TEXT("rig-transform"));
		for (const TCHAR* Required : { TEXT("role"), TEXT("translation"), TEXT("rotation"), TEXT("scale") })
			if (!Seen.Contains(Required)) ErrorAt(Head, FString::Printf(TEXT("rig-transform requires :%s"), Required));
		if (!Role.IsEmpty() && !ParseTransformRole(Role, Transform.Role)) ErrorAt(Head, FString::Printf(TEXT("invalid transform role '%s'"), *Role));
		Transform.Value = FTransform(Rotation, Translation, Scale);
		Element.Transforms.Add(MoveTemp(Transform));
	}

	void ParseHierarchyState(const FAnimLangToken& Open, const FAnimLangToken& Head, FRigHierarchyElementAST& Element)
	{
		FRigHierarchyStateAST State; State.Location = Head.Span; FString Kind;
		TSet<FString> Seen;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key; if (!NextKeyword(Seen, Key)) { ErrorAt(Current(), TEXT("Expected rig-state property")); SkipValue(); continue; }
			if (Key == TEXT("kind")) ReadIdentifier(Kind, Key); else if (Key == TEXT("role")) ReadIdentifier(State.Role, Key);
			else if (Key == TEXT("type")) ReadIdentifier(State.Type, Key); else if (Key == TEXT("bool")) ReadBool(State.bBoolValue, Key);
			else if (Key == TEXT("serialized")) ReadString(State.SerializedValue, Key);
			else if (Key == TEXT("integer")) { int32 V=0; if(ReadInt(V, Key)) State.IntegerValue=V; }
			else if (Key == TEXT("number")) ReadNumber(State.NumberValue, Key);
			else if (Key == TEXT("components")) ReadNumberList(State.Components, -1, Key);
			else { ErrorAt(Current(), FString::Printf(TEXT("Unsupported rig-state property :%s"), *Key)); SkipValue(); }
		}
		ConsumeClose(Open, TEXT("rig-state"));
		if (!Seen.Contains(TEXT("kind"))) ErrorAt(Head, TEXT("rig-state requires :kind"));
		if (!Seen.Contains(TEXT("role"))) ErrorAt(Head, TEXT("rig-state requires :role"));
		if (!Seen.Contains(TEXT("type"))) ErrorAt(Head, TEXT("rig-state requires :type"));
		if (Kind == TEXT("bone-type")) State.Kind=ERigHierarchyStateKind::BoneType; else if(Kind==TEXT("curve")) State.Kind=ERigHierarchyStateKind::Curve;
		else if(Kind==TEXT("control-settings")) State.Kind=ERigHierarchyStateKind::ControlSettings; else if(Kind==TEXT("control-value")) State.Kind=ERigHierarchyStateKind::ControlValue;
		else if(Kind==TEXT("preferred-euler")) State.Kind=ERigHierarchyStateKind::PreferredEuler; else if(!Kind.IsEmpty()) ErrorAt(Head, FString::Printf(TEXT("invalid hierarchy state kind '%s'"), *Kind));

		const TSet<FString> PayloadKeys = {
			TEXT("bool"), TEXT("integer"), TEXT("number"), TEXT("components"), TEXT("serialized") };
		auto RequireRole = [this, &Head, &State](std::initializer_list<const TCHAR*> Roles)
		{
			for (const TCHAR* Role : Roles) if (State.Role == Role) return;
			ErrorAt(Head, FString::Printf(TEXT("rig-state kind has invalid role '%s'"), *State.Role));
		};
		auto ValidatePayload = [this, &Head, &Seen, &PayloadKeys](
			std::initializer_list<const TCHAR*> Required, const TCHAR* Context)
		{
			TSet<FString> Allowed;
			for (const TCHAR* Key : Required)
			{
				Allowed.Add(Key);
				if (!Seen.Contains(Key)) ErrorAt(Head, FString::Printf(TEXT("%s requires :%s"), Context, Key));
			}
			for (const FString& Key : PayloadKeys)
			{
				if (Seen.Contains(Key) && !Allowed.Contains(Key))
					ErrorAt(Head, FString::Printf(TEXT("%s does not allow :%s"), Context, *Key));
			}
		};
		auto ValidateElementKind = [this, &Head, &Element](const ERigHierarchyElementKind Expected, const TCHAR* Context)
		{
			if (Element.Kind != Expected) ErrorAt(Head, FString::Printf(TEXT("%s is not valid for this hierarchy element kind"), Context));
		};

		if (Kind == TEXT("bone-type"))
		{
			ValidateElementKind(ERigHierarchyElementKind::Bone, TEXT("bone-type state"));
			RequireRole({TEXT("initial")});
			ValidatePayload({}, TEXT("bone-type state"));
#if ENGINE_MAJOR_VERSION >= 5
			if (StaticEnum<ERigBoneType>()->GetValueByNameString(State.Type) == INDEX_NONE)
				ErrorAt(Head, FString::Printf(TEXT("bone-type state has invalid type '%s'"), *State.Type));
#else
			ErrorAt(Head, TEXT("[UNSUPPORTED:UE4TypedRigSnapshot] typed Rig hierarchy state requires Unreal Engine 5"));
#endif
		}
		else if (Kind == TEXT("curve"))
		{
			ValidateElementKind(ERigHierarchyElementKind::Curve, TEXT("curve state"));
			RequireRole({TEXT("initial")});
			if (State.Type != TEXT("Float")) ErrorAt(Head, TEXT("curve state requires type 'Float'"));
			ValidatePayload({TEXT("number"), TEXT("bool")}, TEXT("curve state"));
		}
		else if (Kind == TEXT("control-settings"))
		{
			ValidateElementKind(ERigHierarchyElementKind::Control, TEXT("control-settings state"));
			RequireRole({TEXT("initial")});
			ValidatePayload({TEXT("serialized")}, TEXT("control-settings state"));
#if ENGINE_MAJOR_VERSION >= 5
			if (StaticEnum<ERigControlType>()->GetValueByNameString(State.Type) == INDEX_NONE)
				ErrorAt(Head, FString::Printf(TEXT("control-settings state has invalid type '%s'"), *State.Type));
#else
			ErrorAt(Head, TEXT("[UNSUPPORTED:UE4TypedRigSnapshot] typed Control Rig settings require Unreal Engine 5"));
#endif
		}
		else if (Kind == TEXT("control-value"))
		{
			ValidateElementKind(ERigHierarchyElementKind::Control, TEXT("control-value state"));
#if ENGINE_MAJOR_VERSION >= 5
			RequireRole({TEXT("current"), TEXT("initial"), TEXT("minimum"), TEXT("maximum")});
			const int64 ControlTypeValue = StaticEnum<ERigControlType>()->GetValueByNameString(State.Type);
			if (ControlTypeValue == INDEX_NONE)
			{
				ErrorAt(Head, FString::Printf(TEXT("control-value state has invalid type '%s'"), *State.Type));
			}
			else
			{
				switch (static_cast<ERigControlType>(ControlTypeValue))
				{
				case ERigControlType::Bool: ValidatePayload({TEXT("bool")}, TEXT("control-value Bool state")); break;
				case ERigControlType::Integer: ValidatePayload({TEXT("integer")}, TEXT("control-value Integer state")); break;
				case ERigControlType::Float:
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
				case ERigControlType::ScaleFloat:
#endif
					ValidatePayload({TEXT("number")}, TEXT("control-value scalar state")); break;
				case ERigControlType::Vector2D:
					ValidatePayload({TEXT("components")}, TEXT("control-value Vector2D state"));
					if (State.Components.Num() != 2) ErrorAt(Head, TEXT("control-value Vector2D state requires 2 components")); break;
				case ERigControlType::Position:
				case ERigControlType::Scale:
				case ERigControlType::Rotator:
					ValidatePayload({TEXT("components")}, TEXT("control-value vector state"));
					if (State.Components.Num() != 3) ErrorAt(Head, TEXT("control-value vector state requires 3 components")); break;
				case ERigControlType::Transform:
					ValidatePayload({TEXT("components")}, TEXT("control-value Transform state"));
					if (State.Components.Num() != 10) ErrorAt(Head, TEXT("control-value Transform state requires 10 components")); break;
				case ERigControlType::TransformNoScale:
					ValidatePayload({TEXT("components")}, TEXT("control-value TransformNoScale state"));
					if (State.Components.Num() != 7) ErrorAt(Head, TEXT("control-value TransformNoScale state requires 7 components")); break;
				case ERigControlType::EulerTransform:
					ValidatePayload({TEXT("components")}, TEXT("control-value EulerTransform state"));
					if (State.Components.Num() != 9) ErrorAt(Head, TEXT("control-value EulerTransform state requires 9 components")); break;
				}
			}
#else
			RequireRole({TEXT("current"), TEXT("initial"), TEXT("minimum"), TEXT("maximum")});
			ErrorAt(Head, TEXT("[UNSUPPORTED:UE4TypedRigSnapshot] typed Control Rig values require Unreal Engine 5"));
#endif
		}
		else if (Kind == TEXT("preferred-euler"))
		{
			ValidateElementKind(ERigHierarchyElementKind::Control, TEXT("preferred-euler state"));
			RequireRole({TEXT("current"), TEXT("initial")});
			ValidatePayload({TEXT("components")}, TEXT("preferred-euler state"));
			if (State.Components.Num() != 3) ErrorAt(Head, TEXT("preferred-euler state requires 3 components"));
#if ENGINE_MAJOR_VERSION >= 5
			if (StaticEnum<EEulerRotationOrder>()->GetValueByNameString(State.Type) == INDEX_NONE)
				ErrorAt(Head, FString::Printf(TEXT("preferred-euler state has invalid type '%s'"), *State.Type));
#else
			ErrorAt(Head, TEXT("[UNSUPPORTED:UE4TypedRigSnapshot] preferred Euler state requires Unreal Engine 5"));
#endif
		}
#if ENGINE_MAJOR_VERSION >= 5
		if (State.Kind == ERigHierarchyStateKind::ControlSettings && Seen.Contains(TEXT("serialized")))
		{
			FRigControlSettings ParsedSettings;
			UScriptStruct* SettingsStruct = FRigControlSettings::StaticStruct();
			const FString TrimmedSettings = State.SerializedValue.TrimStartAndEnd();
			const bool bHasStructEnvelope = TrimmedSettings.StartsWith(TEXT("("))
				&& TrimmedSettings.EndsWith(TEXT(")"));
			const TCHAR* Remainder = SettingsStruct && bHasStructEnvelope
				? SettingsStruct->ImportText(*State.SerializedValue, &ParsedSettings, nullptr,
					PPF_None, nullptr, TEXT("FRigControlSettings"))
				: nullptr;
			while (Remainder && FChar::IsWhitespace(*Remainder)) ++Remainder;
			if (!Remainder || *Remainder != TEXT('\0'))
			{
				ErrorAt(Head, TEXT("rig-state control-settings has invalid FRigControlSettings payload"));
			}
			else
			{
				const FString SerializedControlType = StaticEnum<ERigControlType>()->GetNameStringByValue(
					static_cast<int64>(ParsedSettings.ControlType));
				if (SerializedControlType != State.Type)
				{
					ErrorAt(Head, FString::Printf(
						TEXT("control-settings serialized ControlType '%s' does not match state type '%s'"),
						*SerializedControlType, *State.Type));
				}
				FString CanonicalSettings;
				SettingsStruct->ExportText(CanonicalSettings, &ParsedSettings, &ParsedSettings, nullptr, PPF_None, nullptr);
				State.SerializedValue = MoveTemp(CanonicalSettings);
			}
		}
#endif
		if (Element.States.ContainsByPredicate([&State](const FRigHierarchyStateAST& Existing)
			{ return Existing.Kind == State.Kind && Existing.Role == State.Role; }))
		{
			ErrorAt(Head, FString::Printf(TEXT("Duplicate hierarchy state kind-role '%s/%s'"), *Kind, *State.Role));
		}
		Element.States.Add(MoveTemp(State));
	}

	void ParseHierarchyMetadata(const FAnimLangToken& Open, const FAnimLangToken& Head, FRigHierarchyElementAST& Element)
	{
		FRigHierarchyMetadataAST Metadata; Metadata.Location=Head.Span; FString Kind;
		TSet<FString> Seen;
		while(!AtEnd()&&!Check(EAnimLangTokenType::RParen))
		{
			FString Key; if(!NextKeyword(Seen,Key)){ErrorAt(Current(),TEXT("Expected rig-metadata property"));SkipValue();continue;}
			if(Key==TEXT("name"))ReadString(Metadata.Name,Key); else if(Key==TEXT("kind"))ReadIdentifier(Kind,Key);
			else if(Key==TEXT("bools"))ReadBoolList(Metadata.BoolValues,Key);
			else if(Key==TEXT("integers"))ReadIntegerList(Metadata.IntegerValues,Key);
			else if(Key==TEXT("numbers"))ReadNumberList(Metadata.NumberValues,-1,Key);
			else if(Key==TEXT("strings"))ReadStringList(Metadata.StringValues,Key);
			else if(Key==TEXT("vectors")||Key==TEXT("rotators")||Key==TEXT("quats")||Key==TEXT("transforms")||Key==TEXT("colors"))
			{
				const int32 Arity=Key==TEXT("quats")||Key==TEXT("colors")?4:Key==TEXT("transforms")?10:3; TArray<TArray<double>> Tuples;
				if(ReadTupleList(Tuples,Arity,Key)) for(const TArray<double>& V:Tuples)
				{
					if(Key==TEXT("vectors"))Metadata.VectorValues.Add(FVector(V[0],V[1],V[2]));
					else if(Key==TEXT("rotators"))Metadata.RotatorValues.Add(FRotator(V[0],V[1],V[2]));
					else if(Key==TEXT("quats"))Metadata.QuatValues.Add(FQuat(V[0],V[1],V[2],V[3]));
					else if(Key==TEXT("colors"))Metadata.ColorValues.Add(FLinearColor(V[0],V[1],V[2],V[3]));
					else Metadata.TransformValues.Add(FTransform(FQuat(V[3],V[4],V[5],V[6]),FVector(V[0],V[1],V[2]),FVector(V[7],V[8],V[9])));
				}
			}
			else { ErrorAt(Current(),FString::Printf(TEXT("Unsupported rig-metadata property :%s"),*Key));SkipValue(); }
		}
		ConsumeClose(Open,TEXT("rig-metadata"));
		if(Metadata.Name.IsEmpty())ErrorAt(Head,TEXT("rig-metadata requires :name")); if(!Seen.Contains(TEXT("kind")))ErrorAt(Head,TEXT("rig-metadata requires :kind"));
		static const TMap<FString,ERigHierarchyMetadataValueKind> Kinds={
			{TEXT("bool"),ERigHierarchyMetadataValueKind::Bool},{TEXT("integer"),ERigHierarchyMetadataValueKind::Integer},{TEXT("float"),ERigHierarchyMetadataValueKind::Float},{TEXT("name"),ERigHierarchyMetadataValueKind::Name},
			{TEXT("vector"),ERigHierarchyMetadataValueKind::Vector},{TEXT("rotator"),ERigHierarchyMetadataValueKind::Rotator},{TEXT("quat"),ERigHierarchyMetadataValueKind::Quat},{TEXT("transform"),ERigHierarchyMetadataValueKind::Transform},
			{TEXT("linear-color"),ERigHierarchyMetadataValueKind::LinearColor},{TEXT("element-key"),ERigHierarchyMetadataValueKind::ElementKey},{TEXT("bool-array"),ERigHierarchyMetadataValueKind::BoolArray},
			{TEXT("integer-array"),ERigHierarchyMetadataValueKind::IntegerArray},{TEXT("float-array"),ERigHierarchyMetadataValueKind::FloatArray},{TEXT("name-array"),ERigHierarchyMetadataValueKind::NameArray},
			{TEXT("vector-array"),ERigHierarchyMetadataValueKind::VectorArray},{TEXT("rotator-array"),ERigHierarchyMetadataValueKind::RotatorArray},{TEXT("quat-array"),ERigHierarchyMetadataValueKind::QuatArray},
			{TEXT("transform-array"),ERigHierarchyMetadataValueKind::TransformArray},{TEXT("linear-color-array"),ERigHierarchyMetadataValueKind::LinearColorArray},{TEXT("element-key-array"),ERigHierarchyMetadataValueKind::ElementKeyArray}};
		if(const ERigHierarchyMetadataValueKind* Parsed=Kinds.Find(Kind))Metadata.Kind=*Parsed; else if(!Kind.IsEmpty())ErrorAt(Head,FString::Printf(TEXT("invalid metadata kind '%s'"),*Kind));
		const FString ValueKey = Metadata.Kind==ERigHierarchyMetadataValueKind::Bool||Metadata.Kind==ERigHierarchyMetadataValueKind::BoolArray?TEXT("bools")
			:Metadata.Kind==ERigHierarchyMetadataValueKind::Integer||Metadata.Kind==ERigHierarchyMetadataValueKind::IntegerArray?TEXT("integers")
			:Metadata.Kind==ERigHierarchyMetadataValueKind::Float||Metadata.Kind==ERigHierarchyMetadataValueKind::FloatArray?TEXT("numbers")
			:Metadata.Kind==ERigHierarchyMetadataValueKind::Name||Metadata.Kind==ERigHierarchyMetadataValueKind::NameArray||Metadata.Kind==ERigHierarchyMetadataValueKind::ElementKey||Metadata.Kind==ERigHierarchyMetadataValueKind::ElementKeyArray?TEXT("strings")
			:Metadata.Kind==ERigHierarchyMetadataValueKind::Vector||Metadata.Kind==ERigHierarchyMetadataValueKind::VectorArray?TEXT("vectors")
			:Metadata.Kind==ERigHierarchyMetadataValueKind::Rotator||Metadata.Kind==ERigHierarchyMetadataValueKind::RotatorArray?TEXT("rotators")
			:Metadata.Kind==ERigHierarchyMetadataValueKind::Quat||Metadata.Kind==ERigHierarchyMetadataValueKind::QuatArray?TEXT("quats")
			:Metadata.Kind==ERigHierarchyMetadataValueKind::Transform||Metadata.Kind==ERigHierarchyMetadataValueKind::TransformArray?TEXT("transforms"):TEXT("colors");
		if(!Seen.Contains(ValueKey))ErrorAt(Head,FString::Printf(TEXT("rig-metadata kind '%s' requires :%s"),*Kind,*ValueKey));
		for (const TCHAR* Candidate : { TEXT("bools"), TEXT("integers"), TEXT("numbers"), TEXT("strings"), TEXT("vectors"), TEXT("rotators"), TEXT("quats"), TEXT("transforms"), TEXT("colors") })
		{
			if (ValueKey != Candidate && Seen.Contains(Candidate))
				ErrorAt(Head, FString::Printf(TEXT("rig-metadata kind '%s' does not allow :%s"), *Kind, Candidate));
		}
		const bool bArray=Kind.EndsWith(TEXT("-array"));
		const int32 Count=Metadata.BoolValues.Num()+Metadata.IntegerValues.Num()+Metadata.NumberValues.Num()+Metadata.StringValues.Num()+Metadata.VectorValues.Num()+Metadata.RotatorValues.Num()+Metadata.QuatValues.Num()+Metadata.TransformValues.Num()+Metadata.ColorValues.Num();
		if(!bArray&&Count!=1)ErrorAt(Head,FString::Printf(TEXT("rig-metadata kind '%s' requires exactly one value"),*Kind));
		Element.Metadata.Add(MoveTemp(Metadata));
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
		else if (Value == TEXT("visible")) OutDirection = ERigPinDirection::Visible;
		else if (Value == TEXT("hidden")) OutDirection = ERigPinDirection::Hidden;
		else if (Value == TEXT("invalid")) OutDirection = ERigPinDirection::Invalid;
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
		if (Head.Value == TEXT("rig-call")) Node.Kind = ERigNodeKind::Call;
		else if (Head.Value == TEXT("rig-variable-node")) Node.Kind = ERigNodeKind::Variable;
		else if (Head.Value == TEXT("rig-comment")) Node.Kind = ERigNodeKind::Comment;
		else if (Head.Value == TEXT("rig-reroute")) Node.Kind = ERigNodeKind::Reroute;
		else if (Head.Value == TEXT("rig-entry-node")) Node.Kind = ERigNodeKind::Entry;
		else if (Head.Value == TEXT("rig-return-node")) Node.Kind = ERigNodeKind::Return;
		else if (Head.Value == TEXT("rig-collapse")) Node.Kind = ERigNodeKind::Collapse;
		else if (Head.Value == TEXT("rig-dispatch")) Node.Kind = ERigNodeKind::Dispatch;
		else if (Head.Value == TEXT("rig-aggregate")) Node.Kind = ERigNodeKind::Aggregate;
		else if (Head.Value == TEXT("rig-invoke-entry")) Node.Kind = ERigNodeKind::InvokeEntry;
		else Node.Kind = ERigNodeKind::Unit;
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
			else if (Key == TEXT("function-identifier-host"))
				ReadString(Node.FunctionIdentifier.HostObject, Key);
			else if (Key == TEXT("function-library-node-path"))
				ReadString(Node.FunctionIdentifier.LibraryNodePath, Key);
			else if (Key == TEXT("contained-graph-id") || Key == TEXT("contained-graph"))
			{
				if (!Node.ContainedGraphStableId.IsEmpty())
					ErrorAt(Current(), TEXT("Node declares both :contained-graph-id and legacy :contained-graph"));
				ReadString(Node.ContainedGraphStableId, Key);
			}
			else if (Key == TEXT("injected")) ReadBool(Node.bInjected, Key);
			else if (Key == TEXT("injection-owner-pin")) ReadString(Node.InjectionOwnerPin, Key);
			else if (Key == TEXT("injection-order")) ReadInt(Node.InjectionOrder, Key);
			else if (Key == TEXT("injected-as-input")) ReadBool(Node.bInjectedAsInput, Key);
			else if (Key == TEXT("injection-input-pin")) ReadString(Node.InjectionInputPin, Key);
			else if (Key == TEXT("injection-output-pin")) ReadString(Node.InjectionOutputPin, Key);
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
		if (Node.Kind != ERigNodeKind::Call && Node.ClassPath.IsEmpty()) ErrorAt(Head, Head.Value + TEXT(" requires :class"));
		if (Node.Kind == ERigNodeKind::Call && Node.FunctionName.IsEmpty()) ErrorAt(Head, TEXT("rig-call requires :function"));
		if (Node.Kind == ERigNodeKind::Call && Node.FunctionIdentifier.IsSet()
			&& !Node.FunctionIdentifier.IsComplete())
			ErrorAt(Head, TEXT("rig-call function identifier requires both host and library-node path"));
		if (!Node.ContainedGraphStableId.IsEmpty()
			&& Node.Kind != ERigNodeKind::Collapse && Node.Kind != ERigNodeKind::Aggregate)
		{
			ErrorAt(Head, FString::Printf(TEXT("Node '%s' kind cannot own a contained graph"), *Node.StableId));
		}
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

	void ParseGraphMember(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head,
		FRigGraphAST& Graph)
	{
		if (Head.Value == TEXT("rig-unit") || Head.Value == TEXT("rig-call")
			|| Head.Value == TEXT("rig-variable-node") || Head.Value == TEXT("rig-comment")
			|| Head.Value == TEXT("rig-reroute") || Head.Value == TEXT("rig-entry-node")
			|| Head.Value == TEXT("rig-return-node") || Head.Value == TEXT("rig-collapse")
			|| Head.Value == TEXT("rig-dispatch") || Head.Value == TEXT("rig-aggregate")
			|| Head.Value == TEXT("rig-invoke-entry"))
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
		ParseGraphMember(Open, Head, Graph);
	}

	FRigCallableArgumentAST ParseArgument(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head)
	{
		FRigCallableArgumentAST Argument;
		Argument.Location = Head.Span;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected rig-argument property"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("name")) ReadString(Argument.Name, Key);
			else if (Key == TEXT("direction"))
			{
				FString Value;
				if (ReadIdentifier(Value, Key) && !ParseDirection(Value, Argument.Direction))
				{
					ErrorAt(Tokens[Pos - 1], FString::Printf(TEXT("Unsupported argument direction '%s'"), *Value));
				}
			}
			else if (Key == TEXT("cpp-type")) ReadString(Argument.Type.CPPType, Key);
			else if (Key == TEXT("cpp-type-object")) ReadString(Argument.Type.CPPTypeObject, Key);
			else if (Key == TEXT("container-type")) ReadString(Argument.Type.ContainerType, Key);
			else if (Key == TEXT("default")) ReadString(Argument.DefaultValue, Key);
			else if (Key == TEXT("execute-context")) ReadBool(Argument.bExecuteContext, Key);
			else if (Key == TEXT("constant")) ReadBool(Argument.bConstant, Key);
			else if (Key == TEXT("input-variable")) ReadBool(Argument.bInputVariable, Key);
			else
			{
				ErrorAt(Current(), FString::Printf(TEXT("Unsupported rig-argument property :%s"), *Key));
				SkipValue();
			}
		}
		ConsumeClose(Open, TEXT("rig-argument"));
		if (Argument.Name.IsEmpty()) ErrorAt(Head, TEXT("rig-argument requires :name"));
		if (Argument.Type.CPPType.IsEmpty()) ErrorAt(Head, TEXT("rig-argument requires :cpp-type"));
		return Argument;
	}

	FRigGraphVariableAST ParseLocalVariable(const FAnimLangToken& Open, const FAnimLangToken& Head)
	{
		FRigGraphVariableAST Variable;
		Variable.Location = Head.Span;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected rig-local-variable property"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("guid")) ReadString(Variable.Guid, Key);
			else if (Key == TEXT("name")) ReadString(Variable.Name, Key);
			else if (Key == TEXT("cpp-type")) ReadString(Variable.Type.CPPType, Key);
			else if (Key == TEXT("cpp-type-object")) ReadString(Variable.Type.CPPTypeObject, Key);
			else if (Key == TEXT("cpp-type-object-path")) ReadString(Variable.CPPTypeObjectPath, Key);
			else if (Key == TEXT("container-type")) ReadString(Variable.Type.ContainerType, Key);
			else if (Key == TEXT("default")) ReadString(Variable.DefaultValue, Key);
			else if (Key == TEXT("category")) ReadString(Variable.Category, Key);
			else if (Key == TEXT("tooltip")) ReadString(Variable.Tooltip, Key);
			else if (Key == TEXT("exposed-on-spawn")) ReadBool(Variable.bExposedOnSpawn, Key);
			else if (Key == TEXT("expose-to-cinematics")) ReadBool(Variable.bExposeToCinematics, Key);
			else if (Key == TEXT("public")) ReadBool(Variable.bPublic, Key);
			else if (Key == TEXT("private")) ReadBool(Variable.bPrivate, Key);
			else { ErrorAt(Current(), FString::Printf(TEXT("Unsupported rig-local-variable property :%s"), *Key)); SkipValue(); }
		}
		ConsumeClose(Open, TEXT("rig-local-variable"));
		if (Variable.Guid.IsEmpty())
		{
			ErrorAt(Head, TEXT("rig-local-variable requires :guid"));
		}
		else
		{
			FGuid ParsedGuid;
				if (Variable.Guid.Len() != 36
					|| !FGuid::ParseExact(Variable.Guid, EGuidFormats::DigitsWithHyphens, ParsedGuid)
					|| !ParsedGuid.IsValid())
				ErrorAt(Head, TEXT("rig-local-variable requires a non-zero hyphenated GUID"));
			else
				Variable.Guid = ParsedGuid.ToString(EGuidFormats::DigitsWithHyphens);
		}
		if (FName(*Variable.Name).IsNone()) ErrorAt(Head, TEXT("rig-local-variable requires a non-None :name"));
		if (Variable.Type.CPPType.IsEmpty()) ErrorAt(Head, TEXT("rig-local-variable requires :cpp-type"));
		auto ValidateTextSerialization = [this, &Head](const FString& Serialized, const TCHAR* Field)
		{
			if (Serialized.IsEmpty()) return;
			FText ParsedText;
			const TCHAR* Remainder = FTextStringHelper::ReadFromBuffer(*Serialized, ParsedText);
			while (Remainder && FChar::IsWhitespace(*Remainder)) ++Remainder;
			if (!Remainder || *Remainder != TEXT('\0'))
				ErrorAt(Head, FString::Printf(TEXT("rig-local-variable has invalid :%s FText serialization"), Field));
		};
		ValidateTextSerialization(Variable.Category, TEXT("category"));
		ValidateTextSerialization(Variable.Tooltip, TEXT("tooltip"));
		return Variable;
	}

	FRigExternalVariableAST ParseExternalVariable(const FAnimLangToken& Open, const FAnimLangToken& Head)
	{
		FRigExternalVariableAST Variable;
		Variable.Location = Head.Span;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected rig-external-variable property"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("guid")) ReadString(Variable.Guid, Key);
			else if (Key == TEXT("name")) ReadString(Variable.Name, Key);
			else if (Key == TEXT("cpp-type")) ReadString(Variable.Type.CPPType, Key);
			else if (Key == TEXT("cpp-type-object")) ReadString(Variable.Type.CPPTypeObject, Key);
			else if (Key == TEXT("container-type")) ReadString(Variable.Type.ContainerType, Key);
			else if (Key == TEXT("public")) ReadBool(Variable.bPublic, Key);
			else if (Key == TEXT("read-only")) ReadBool(Variable.bReadOnly, Key);
			else { ErrorAt(Current(), FString::Printf(TEXT("Unsupported rig-external-variable property :%s"), *Key)); SkipValue(); }
		}
		ConsumeClose(Open, TEXT("rig-external-variable"));
		if (Variable.Name.IsEmpty()) ErrorAt(Head, TEXT("rig-external-variable requires :name"));
		if (Variable.Type.CPPType.IsEmpty()) ErrorAt(Head, TEXT("rig-external-variable requires :cpp-type"));
		return Variable;
	}

	FRigFunctionDependencyAST ParseDependency(const FAnimLangToken& Open, const FAnimLangToken& Head)
	{
		FRigFunctionDependencyAST Dependency;
		Dependency.Location = Head.Span;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected rig-dependency property"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("host")) ReadString(Dependency.HostObject, Key);
			else if (Key == TEXT("library-node-path")) ReadString(Dependency.LibraryNodePath, Key);
			else if (Key == TEXT("hash")) ReadUInt32(Dependency.Hash, Key);
			else { ErrorAt(Current(), FString::Printf(TEXT("Unsupported rig-dependency property :%s"), *Key)); SkipValue(); }
		}
		ConsumeClose(Open, TEXT("rig-dependency"));
		if (Dependency.HostObject.IsEmpty()) ErrorAt(Head, TEXT("rig-dependency requires :host"));
		if (Dependency.LibraryNodePath.IsEmpty()) ErrorAt(Head, TEXT("rig-dependency requires :library-node-path"));
		return Dependency;
	}

	void ParseGraph(
		const FAnimLangToken& Open,
		const FAnimLangToken& Head,
		TArray<FRigGraphAST>& Graphs)
	{
		FRigGraphAST Graph;
		Graph.Location = Head.Span;
		TSet<FString> SeenProperties;
		while (!AtEnd() && !Check(EAnimLangTokenType::RParen))
		{
			if (Check(EAnimLangTokenType::LParen))
			{
				const FAnimLangToken ChildOpen = Advance();
				if (!Check(EAnimLangTokenType::Identifier))
				{
					ErrorAt(Current(), TEXT("Expected graph child form name"));
					SkipFormBody();
					continue;
				}
				const FAnimLangToken ChildHead = Advance();
				if (ChildHead.Value == TEXT("rig-local-variable"))
				{
					Graph.LocalVariables.Add(ParseLocalVariable(ChildOpen, ChildHead));
				}
				else ParseGraphMember(ChildOpen, ChildHead, Graph);
				continue;
			}
			FString Key;
			if (!NextKeyword(SeenProperties, Key))
			{
				ErrorAt(Current(), TEXT("Expected graph property or graph member"));
				SkipValue();
				continue;
			}
			if (Key == TEXT("id")) ReadString(Graph.StableId, Key);
			else if (Key == TEXT("editor-guid")) ReadString(Graph.EditorGuid, Key);
			else if (Key == TEXT("role")) ReadString(Graph.Role, Key);
			else if (Key == TEXT("parent-id")) ReadString(Graph.ParentStableId, Key);
			else ReadUnknownProperty(Key, Graph.Properties);
		}
		ConsumeClose(Open, TEXT("define-rig-graph"));
		if (Graph.StableId.IsEmpty()) ErrorAt(Head, TEXT("define-rig-graph requires :id"));
		if (Graph.Role.IsEmpty()) ErrorAt(Head, TEXT("define-rig-graph requires :role"));
		Graphs.Add(MoveTemp(Graph));
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
				const FAnimLangToken ChildOpen = Advance();
				if (!Check(EAnimLangTokenType::Identifier))
				{
					ErrorAt(Current(), TEXT("Expected function child form name"));
					SkipFormBody();
					continue;
				}
				const FAnimLangToken ChildHead = Advance();
				if (ChildHead.Value == TEXT("rig-argument"))
				{
					FRigCallableArgumentAST Argument = ParseArgument(ChildOpen, ChildHead);
					Function.Arguments.Add(Argument);
					if (Argument.Direction == ERigPinDirection::Output) Function.Outputs.Add(MoveTemp(Argument));
					else Function.Inputs.Add(MoveTemp(Argument));
				}
				else if (ChildHead.Value == TEXT("rig-external-variable"))
					Function.ExternalVariables.Add(ParseExternalVariable(ChildOpen, ChildHead));
				else if (ChildHead.Value == TEXT("rig-dependency"))
					Function.Dependencies.Add(ParseDependency(ChildOpen, ChildHead));
				else ParseGraphMember(ChildOpen, ChildHead, Function.Graph);
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
			else if (Key == TEXT("graph-id")) ReadString(Function.GraphStableId, Key);
			else if (Key == TEXT("identifier-host")) ReadString(Function.FunctionIdentifier.HostObject, Key);
			else if (Key == TEXT("library-node-path")) ReadString(Function.FunctionIdentifier.LibraryNodePath, Key);
			else ReadUnknownProperty(Key, Function.Properties);
		}
		ConsumeClose(Open, TEXT("define-rig-function"));
		if (Function.Name.IsEmpty()) ErrorAt(Head, TEXT("define-rig-function requires a name"));
		if (Function.StableId.IsEmpty()) ErrorAt(Head, TEXT("define-rig-function requires :id"));
		if (Function.Visibility.IsEmpty()) ErrorAt(Head, TEXT("define-rig-function requires :visibility"));
		else if (Function.Visibility != TEXT("public") && Function.Visibility != TEXT("internal"))
			ErrorAt(Head, FString::Printf(TEXT("define-rig-function has invalid visibility '%s'"), *Function.Visibility));
		if (Function.FunctionIdentifier.IsSet() && !Function.FunctionIdentifier.IsComplete())
			ErrorAt(Head, TEXT("define-rig-function identifier requires both host and library-node path"));
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
				const FAnimLangToken ChildOpen = Advance();
				if (!Check(EAnimLangTokenType::Identifier))
				{
					ErrorAt(Current(), TEXT("Expected entry child form name"));
					SkipFormBody();
					continue;
				}
				const FAnimLangToken ChildHead = Advance();
				if (ChildHead.Value == TEXT("rig-argument"))
				{
					FRigCallableArgumentAST Argument = ParseArgument(ChildOpen, ChildHead);
					Entry.Arguments.Add(Argument);
					if (Argument.Direction == ERigPinDirection::Output) Entry.Outputs.Add(MoveTemp(Argument));
					else Entry.Inputs.Add(MoveTemp(Argument));
				}
				else ParseGraphMember(ChildOpen, ChildHead, Entry.Graph);
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
			else if (Key == TEXT("graph-id")) ReadString(Entry.GraphStableId, Key);
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
	if (OutErrors.Num() != 0)
	{
		return nullptr;
	}

	FRigLangParserImpl Parser(Tokens, OutErrors);
	return Parser.ParseModule();
}
