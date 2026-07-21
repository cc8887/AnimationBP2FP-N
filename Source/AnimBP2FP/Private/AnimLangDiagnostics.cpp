// AnimLangDiagnostics.cpp - Diagnostics & Semantic Analyzer Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangDiagnostics.h"
#include "AnimLangAST.h"

// ========== FAnimLangDiagnostic ==========

FAnimLangDiagnostic::FAnimLangDiagnostic(
	EAnimLangDiagSeverity InSeverity,
	EAnimLangDiagCategory InCategory,
	const FString& InMessage,
	const FAnimLangSourceLoc& InLocation)
	: Severity(InSeverity)
	, Category(InCategory)
	, Message(InMessage)
	, Location(InLocation)
{
}

FAnimLangDiagnostic& FAnimLangDiagnostic::AddRelatedLocation(
	const FAnimLangSourceLoc& RelatedLocation,
	const FString& RelatedMessage)
{
	RelatedLocations.Add(RelatedLocation);
	RelatedMessages.Add(RelatedMessage.IsEmpty() ? TEXT("related") : RelatedMessage);
	return *this;
}

FString FAnimLangDiagnostic::ToString() const
{
	FString SevStr;
	switch (Severity)
	{
	case EAnimLangDiagSeverity::Error:   SevStr = TEXT("ERROR"); break;
	case EAnimLangDiagSeverity::Warning: SevStr = TEXT("WARNING"); break;
	case EAnimLangDiagSeverity::Info:    SevStr = TEXT("INFO"); break;
	case EAnimLangDiagSeverity::Hint:    SevStr = TEXT("HINT"); break;
	}
	
	FString CatStr;
	switch (Category)
	{
	case EAnimLangDiagCategory::Lex:       CatStr = TEXT("lex"); break;
	case EAnimLangDiagCategory::Parse:     CatStr = TEXT("parse"); break;
	case EAnimLangDiagCategory::Type:      CatStr = TEXT("type"); break;
	case EAnimLangDiagCategory::Semantic:  CatStr = TEXT("semantic"); break;
	case EAnimLangDiagCategory::Import:    CatStr = TEXT("import"); break;
	case EAnimLangDiagCategory::RoundTrip: CatStr = TEXT("roundtrip"); break;
	case EAnimLangDiagCategory::Module:     CatStr = TEXT("module"); break;
	case EAnimLangDiagCategory::Capability: CatStr = TEXT("capability"); break;
	}
	
	FString Result = FString::Printf(TEXT("[%s/%s] %s: %s"), *SevStr, *CatStr, *Location.ToString(), *Message);
	
	if (!FixSuggestion.IsEmpty())
	{
		Result += FString::Printf(TEXT("\n  Fix: %s"), *FixSuggestion);
	}
	
	for (int32 i = 0; i < RelatedLocations.Num(); i++)
	{
		FString RelMsg = (i < RelatedMessages.Num()) ? RelatedMessages[i] : TEXT("related");
		Result += FString::Printf(TEXT("\n  %s: %s"), *RelatedLocations[i].ToString(), *RelMsg);
	}
	
	return Result;
}

FString FAnimLangDiagnostic::ToCompactString() const
{
	FString SevStr;
	switch (Severity)
	{
	case EAnimLangDiagSeverity::Error:   SevStr = TEXT("error"); break;
	case EAnimLangDiagSeverity::Warning: SevStr = TEXT("warning"); break;
	case EAnimLangDiagSeverity::Info:    SevStr = TEXT("info"); break;
	case EAnimLangDiagSeverity::Hint:    SevStr = TEXT("hint"); break;
	}
	
	return FString::Printf(TEXT("%s: %s: %s"), *Location.ToString(), *SevStr, *Message);
}

FAnimLangDiagnostic FAnimLangDiagnostic::LexError(const FString& Msg, int32 Line, int32 Col)
{
	FAnimLangDiagnostic D;
	D.Severity = EAnimLangDiagSeverity::Error;
	D.Category = EAnimLangDiagCategory::Lex;
	D.Message = Msg;
	D.Location.Line = Line;
	D.Location.Column = Col;
	return D;
}

FAnimLangDiagnostic FAnimLangDiagnostic::ParseError(const FString& Msg, int32 Line, int32 Col)
{
	FAnimLangDiagnostic D;
	D.Severity = EAnimLangDiagSeverity::Error;
	D.Category = EAnimLangDiagCategory::Parse;
	D.Message = Msg;
	D.Location.Line = Line;
	D.Location.Column = Col;
	return D;
}

FAnimLangDiagnostic FAnimLangDiagnostic::TypeError(const FString& Msg, int32 Line, int32 Col)
{
	FAnimLangDiagnostic D;
	D.Severity = EAnimLangDiagSeverity::Error;
	D.Category = EAnimLangDiagCategory::Type;
	D.Message = Msg;
	D.Location.Line = Line;
	D.Location.Column = Col;
	return D;
}

FAnimLangDiagnostic FAnimLangDiagnostic::SemanticError(const FString& Msg, int32 Line, int32 Col)
{
	FAnimLangDiagnostic D;
	D.Severity = EAnimLangDiagSeverity::Error;
	D.Category = EAnimLangDiagCategory::Semantic;
	D.Message = Msg;
	D.Location.Line = Line;
	D.Location.Column = Col;
	return D;
}

FAnimLangDiagnostic FAnimLangDiagnostic::SemanticWarning(const FString& Msg, int32 Line, int32 Col)
{
	FAnimLangDiagnostic D;
	D.Severity = EAnimLangDiagSeverity::Warning;
	D.Category = EAnimLangDiagCategory::Semantic;
	D.Message = Msg;
	D.Location.Line = Line;
	D.Location.Column = Col;
	return D;
}

// ========== FAnimLangDiagnostics ==========

void FAnimLangDiagnostics::Add(EAnimLangDiagSeverity Sev, EAnimLangDiagCategory Cat, const FString& Msg, int32 Line, int32 Col)
{
	FAnimLangSourceLoc Location;
	Location.Line = Line;
	Location.Column = Col;
	Items.Emplace(Sev, Cat, Msg, Location);
}

void FAnimLangDiagnostics::Add(
	EAnimLangDiagSeverity Sev,
	EAnimLangDiagCategory Cat,
	const FString& Msg,
	const FAnimLangSourceLoc& Location)
{
	Items.Emplace(Sev, Cat, Msg, Location);
}

void FAnimLangDiagnostics::Add(
	EAnimLangDiagSeverity Sev,
	EAnimLangDiagCategory Cat,
	const FString& Msg,
	const FAnimLangSourceLoc& Location,
	const FAnimLangSourceLoc& RelatedLocation,
	const FString& RelatedMessage)
{
	FAnimLangDiagnostic Diagnostic(Sev, Cat, Msg, Location);
	Diagnostic.AddRelatedLocation(RelatedLocation, RelatedMessage);
	Items.Add(MoveTemp(Diagnostic));
}

bool FAnimLangDiagnostics::HasErrors() const
{
	for (const auto& D : Items)
	{
		if (D.Severity == EAnimLangDiagSeverity::Error) return true;
	}
	return false;
}

bool FAnimLangDiagnostics::HasWarnings() const
{
	for (const auto& D : Items)
	{
		if (D.Severity == EAnimLangDiagSeverity::Warning) return true;
	}
	return false;
}

int32 FAnimLangDiagnostics::ErrorCount() const
{
	int32 Count = 0;
	for (const auto& D : Items)
	{
		if (D.Severity == EAnimLangDiagSeverity::Error) Count++;
	}
	return Count;
}

int32 FAnimLangDiagnostics::WarningCount() const
{
	int32 Count = 0;
	for (const auto& D : Items)
	{
		if (D.Severity == EAnimLangDiagSeverity::Warning) Count++;
	}
	return Count;
}

FString FAnimLangDiagnostics::ToReport() const
{
	FString Report;
	Report += FString::Printf(TEXT("=== AnimLang Diagnostics: %d errors, %d warnings ===\n"),
		ErrorCount(), WarningCount());
	
	for (const auto& D : Items)
	{
		Report += D.ToString() + TEXT("\n");
	}
	
	return Report;
}

TArray<FAnimLangDiagnostic> FAnimLangDiagnostics::GetErrors() const
{
	TArray<FAnimLangDiagnostic> Result;
	for (const auto& D : Items)
	{
		if (D.Severity == EAnimLangDiagSeverity::Error) Result.Add(D);
	}
	return Result;
}

TArray<FAnimLangDiagnostic> FAnimLangDiagnostics::GetByCategory(EAnimLangDiagCategory Cat) const
{
	TArray<FAnimLangDiagnostic> Result;
	for (const auto& D : Items)
	{
		if (D.Category == Cat) Result.Add(D);
	}
	return Result;
}

// ========== FAnimLangSemanticAnalyzer ==========

void FAnimLangSemanticAnalyzer::Analyze(const TSharedPtr<FAnimGraphAST>& AST, FAnimLangDiagnostics& OutDiag)
{
	if (!AST.IsValid())
	{
		OutDiag.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Semantic, TEXT("Null AST"));
		return;
	}
	
	TSet<FString> DefineNames = CollectDefineNames(AST);
	TSet<FString> VariableNames = CollectVariableNames(AST);
	
	// Check for duplicate defines
	{
		TMap<FString, int32> DefCounts;
		for (const FCachedPoseDef& Def : AST->Defines)
		{
			FString Id = Def.GetIdentifier();
			int32& Count = DefCounts.FindOrAdd(Id);
			Count++;
			if (Count > 1)
			{
				OutDiag.Add(FAnimLangDiagnostic::SemanticError(
					FString::Printf(TEXT("Duplicate define: '%s' (defined %d times)"), *Id, Count),
					0, 0));
			}
		}
	}
	
	// Check define bodies for references
	for (const FCachedPoseDef& Def : AST->Defines)
	{
		FString Path = FString::Printf(TEXT("define[%s]"), *Def.GetIdentifier());
		CheckNodeRefs(Def.Body, DefineNames, VariableNames, Path, OutDiag);
	}
	
	// Check root node
	if (AST->RootNode.IsValid())
	{
		CheckNodeRefs(AST->RootNode, DefineNames, VariableNames, TEXT("root"), OutDiag);
	}
	else
	{
		OutDiag.Add(EAnimLangDiagSeverity::Warning, EAnimLangDiagCategory::Semantic,
			TEXT("No root animation graph node"));
	}
	
	// Check circular defines
	CheckCircularDefines(AST, OutDiag);
}

TSet<FString> FAnimLangSemanticAnalyzer::CollectDefineNames(const TSharedPtr<FAnimGraphAST>& AST)
{
	TSet<FString> Names;
	for (const FCachedPoseDef& Def : AST->Defines)
	{
		Names.Add(Def.GetIdentifier());
	}
	return Names;
}

TSet<FString> FAnimLangSemanticAnalyzer::CollectVariableNames(const TSharedPtr<FAnimGraphAST>& AST)
{
	TSet<FString> Names;
	for (const FVariableDef& Var : AST->Variables)
	{
		Names.Add(Var.Name);
	}
	return Names;
}

void FAnimLangSemanticAnalyzer::CheckNodeRefs(
	const TSharedPtr<FAnimNodeAST>& Node,
	const TSet<FString>& DefineNames,
	const TSet<FString>& VariableNames,
	const FString& Path,
	FAnimLangDiagnostics& OutDiag)
{
	if (!Node.IsValid()) return;
	
	// Check if this is a bare identifier (UseCachedPose reference)
	// Bare identifiers have no properties and no children
	if (Node->Properties.Num() == 0 && Node->Children.Num() == 0)
	{
		FString Id = Node->NodeType;
		
		// Known built-in node types that are leaf nodes
		static TSet<FString> LeafTypes = {
			TEXT("identity-pose"), TEXT("linked-anim-layer"),
			TEXT("pose-snapshot"), TEXT("component-to-local-space"),
			TEXT("local-to-component-space")
		};
		
		if (!LeafTypes.Contains(Id) && !DefineNames.Contains(Id))
		{
			// Could be a variable reference or an unknown node
			// Only warn if it looks like a define reference (has hyphens, PascalCase, etc.)
			bool bLooksLikeDefRef = Id.Contains(TEXT("-")) && FChar::IsUpper(Id[0]);
			if (bLooksLikeDefRef)
			{
				OutDiag.Add(FAnimLangDiagnostic::SemanticWarning(
					FString::Printf(TEXT("Possible unresolved define reference: '%s' at %s"), *Id, *Path),
					0, 0));
			}
		}
	}
	
	// Check (ref "...") in properties
	for (const auto& Pair : Node->Properties)
	{
		if (Pair.Value.StartsWith(TEXT("(ref ")))
		{
			// Extract the ref target: (ref "Node Title")
			FString RefTarget = Pair.Value;
			RefTarget.RemoveFromStart(TEXT("(ref \""));
			RefTarget.RemoveFromEnd(TEXT("\")"));
			
			// We can't resolve blueprint node references at DSL level — just note them
			// This is informational, not an error
		}
	}
	
	// Check state machines
	if (Node->NodeType == TEXT("state-machine"))
	{
		CheckStateMachine(Node, Path, OutDiag);
	}
	
	// Recurse into children
	for (const FNamedChild& Child : Node->Children)
	{
		FString ChildPath = Path + TEXT(".");
		if (!Child.PinName.IsEmpty())
		{
			ChildPath += Child.PinName;
		}
		else if (Child.Node.IsValid())
		{
			ChildPath += Child.Node->NodeType;
		}
		
		CheckNodeRefs(Child.Node, DefineNames, VariableNames, ChildPath, OutDiag);
	}
}

void FAnimLangSemanticAnalyzer::CheckStateMachine(
	const TSharedPtr<FAnimNodeAST>& Node,
	const FString& Path,
	FAnimLangDiagnostics& OutDiag)
{
	if (!Node.IsValid() || Node->NodeType != TEXT("state-machine")) return;
	
	// Get the initial state
	const FString* InitialState = Node->Properties.Find(TEXT("initial"));
	
	// Collect state names from children
	TSet<FString> StateNames;
	for (const FNamedChild& Child : Node->Children)
	{
		if (!Child.PinName.IsEmpty())
		{
			StateNames.Add(Child.PinName);
		}
	}
	
	// Check initial state exists
	if (InitialState)
	{
		// Convert kebab-case to match state names
		FString InitName = *InitialState;
		InitName.RemoveFromStart(TEXT("\""));
		InitName.RemoveFromEnd(TEXT("\""));
		
		// TODO: More sophisticated name matching (space vs hyphen)
	}
	
	// Check transitions reference valid states
	const FString* TransStr = Node->Properties.Find(TEXT("transitions"));
	if (TransStr)
	{
		// Transitions are stored as a serialized string, would need to parse them
		// For now, just verify the property exists
	}
	
	// Check state machine has at least one state
	if (Node->Children.Num() == 0)
	{
		OutDiag.Add(FAnimLangDiagnostic::SemanticWarning(
			FString::Printf(TEXT("State machine at %s has no states"), *Path),
			0, 0));
	}
}

void FAnimLangSemanticAnalyzer::CheckCircularDefines(
	const TSharedPtr<FAnimGraphAST>& AST,
	FAnimLangDiagnostics& OutDiag)
{
	if (AST->Defines.Num() <= 1) return;
	
	// Build dependency graph
	TMap<FString, int32> NameToIdx;
	for (int32 i = 0; i < AST->Defines.Num(); i++)
	{
		NameToIdx.Add(AST->Defines[i].GetIdentifier(), i);
	}
	
	TArray<TSet<int32>> Deps;
	Deps.SetNum(AST->Defines.Num());
	
	// Recursive lambda to find define references in subtree
	TFunction<void(const TSharedPtr<FAnimNodeAST>&, TSet<int32>&)> CollectDeps;
	CollectDeps = [&](const TSharedPtr<FAnimNodeAST>& Node, TSet<int32>& OutDeps)
	{
		if (!Node.IsValid()) return;
		
		const int32* DepIdx = NameToIdx.Find(Node->NodeType);
		if (DepIdx)
		{
			OutDeps.Add(*DepIdx);
		}
		
		for (const FNamedChild& Child : Node->Children)
		{
			CollectDeps(Child.Node, OutDeps);
		}
	};
	
	for (int32 i = 0; i < AST->Defines.Num(); i++)
	{
		CollectDeps(AST->Defines[i].Body, Deps[i]);
		Deps[i].Remove(i);  // Remove self
	}
	
	// DFS cycle detection
	enum class EColor { White, Gray, Black };
	TArray<EColor> Colors;
	Colors.Init(EColor::White, AST->Defines.Num());
	
	TFunction<bool(int32, TArray<int32>&)> HasCycle;
	HasCycle = [&](int32 Node, TArray<int32>& CyclePath) -> bool
	{
		Colors[Node] = EColor::Gray;
		CyclePath.Add(Node);
		
		for (int32 Dep : Deps[Node])
		{
			if (Colors[Dep] == EColor::Gray)
			{
				CyclePath.Add(Dep);
				return true;
			}
			if (Colors[Dep] == EColor::White)
			{
				if (HasCycle(Dep, CyclePath))
				{
					return true;
				}
			}
		}
		
		CyclePath.RemoveAt(CyclePath.Num() - 1);
		Colors[Node] = EColor::Black;
		return false;
	};
	
	for (int32 i = 0; i < AST->Defines.Num(); i++)
	{
		if (Colors[i] == EColor::White)
		{
			TArray<int32> CyclePath;
			if (HasCycle(i, CyclePath))
			{
				FString CycleStr;
				for (int32 Idx : CyclePath)
				{
					if (!CycleStr.IsEmpty()) CycleStr += TEXT(" -> ");
					CycleStr += AST->Defines[Idx].GetIdentifier();
				}
				
				OutDiag.Add(FAnimLangDiagnostic::SemanticError(
					FString::Printf(TEXT("Circular define dependency: %s"), *CycleStr),
					0, 0));
				break;  // One cycle report is enough
			}
		}
	}
}
