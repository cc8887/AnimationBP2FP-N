// AnimLangDiffer.cpp - AST Diff Engine Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangDiffer.h"

// ========== FAnimLangDiffEntry ==========

FString FAnimLangDiffEntry::ToString() const
{
	switch (Op)
	{
	case EAnimLangDiffOp::NodeAdded:
		return FString::Printf(TEXT("+ Node added at %s"), *NodePath);
	case EAnimLangDiffOp::NodeRemoved:
		return FString::Printf(TEXT("- Node removed at %s"), *NodePath);
	case EAnimLangDiffOp::NodeMoved:
		return FString::Printf(TEXT("~ Node moved: %s"), *NodePath);
	case EAnimLangDiffOp::PropertyChanged:
		return FString::Printf(TEXT("~ Property %s.%s: %s -> %s"), *NodePath, *PropertyKey, *OldValue, *NewValue);
	case EAnimLangDiffOp::PropertyAdded:
		return FString::Printf(TEXT("+ Property %s.%s = %s"), *NodePath, *PropertyKey, *NewValue);
	case EAnimLangDiffOp::PropertyRemoved:
		return FString::Printf(TEXT("- Property %s.%s (was %s)"), *NodePath, *PropertyKey, *OldValue);
	case EAnimLangDiffOp::ChildAdded:
		return FString::Printf(TEXT("+ Child added: %s.%s"), *NodePath, *ChildName);
	case EAnimLangDiffOp::ChildRemoved:
		return FString::Printf(TEXT("- Child removed: %s.%s"), *NodePath, *ChildName);
	case EAnimLangDiffOp::ChildReordered:
		return FString::Printf(TEXT("~ Children reordered at %s"), *NodePath);
	case EAnimLangDiffOp::DefineAdded:
		return FString::Printf(TEXT("+ Define added: %s"), *ChildName);
	case EAnimLangDiffOp::DefineRemoved:
		return FString::Printf(TEXT("- Define removed: %s"), *ChildName);
	case EAnimLangDiffOp::DefineBodyChanged:
		return FString::Printf(TEXT("~ Define body changed: %s"), *ChildName);
	case EAnimLangDiffOp::VariableAdded:
		return FString::Printf(TEXT("+ Variable added: %s"), *VariableName);
	case EAnimLangDiffOp::VariableRemoved:
		return FString::Printf(TEXT("- Variable removed: %s"), *VariableName);
	case EAnimLangDiffOp::VariableChanged:
		return FString::Printf(TEXT("~ Variable changed: %s (%s -> %s)"), *VariableName, *OldValue, *NewValue);
	case EAnimLangDiffOp::RootChanged:
		return FString::Printf(TEXT("! Root node type changed: %s -> %s"), *OldValue, *NewValue);
	default:
		return TEXT("Unknown diff op");
	}
}

int32 FAnimLangDiffEntry::GetSeverity() const
{
	switch (Op)
	{
	case EAnimLangDiffOp::PropertyChanged:
	case EAnimLangDiffOp::PropertyAdded:
	case EAnimLangDiffOp::PropertyRemoved:
	case EAnimLangDiffOp::ChildReordered:
		return 0;  // Cosmetic / parameter tweak
	case EAnimLangDiffOp::NodeAdded:
	case EAnimLangDiffOp::NodeRemoved:
	case EAnimLangDiffOp::NodeMoved:
	case EAnimLangDiffOp::ChildAdded:
	case EAnimLangDiffOp::ChildRemoved:
	case EAnimLangDiffOp::DefineAdded:
	case EAnimLangDiffOp::DefineRemoved:
	case EAnimLangDiffOp::DefineBodyChanged:
	case EAnimLangDiffOp::VariableAdded:
	case EAnimLangDiffOp::VariableRemoved:
	case EAnimLangDiffOp::VariableChanged:
		return 1;  // Structural change
	case EAnimLangDiffOp::RootChanged:
		return 2;  // Breaking change
	default:
		return 0;
	}
}

// ========== FAnimLangDiffResult ==========

int32 FAnimLangDiffResult::NumStructuralChanges() const
{
	int32 Count = 0;
	for (const auto& E : Entries)
	{
		if (E.GetSeverity() >= 1) Count++;
	}
	return Count;
}

int32 FAnimLangDiffResult::NumPropertyChanges() const
{
	int32 Count = 0;
	for (const auto& E : Entries)
	{
		if (E.Op == EAnimLangDiffOp::PropertyChanged || 
			E.Op == EAnimLangDiffOp::PropertyAdded || 
			E.Op == EAnimLangDiffOp::PropertyRemoved)
		{
			Count++;
		}
	}
	return Count;
}

FString FAnimLangDiffResult::ToSummary() const
{
	if (!HasChanges())
	{
		return TEXT("No changes detected.");
	}
	return FString::Printf(TEXT("%d changes: %d structural, %d property"),
		Entries.Num(), NumStructuralChanges(), NumPropertyChanges());
}

FString FAnimLangDiffResult::ToDetailedReport() const
{
	FString Report;
	Report += FString::Printf(TEXT("=== AnimLang Diff Report (%d changes) ===\n"), Entries.Num());
	
	for (const auto& E : Entries)
	{
		Report += TEXT("  ") + E.ToString() + TEXT("\n");
	}
	
	return Report;
}

// ========== FAnimLangDiffer ==========

FAnimLangDiffResult FAnimLangDiffer::Diff(
	const TSharedPtr<FAnimGraphAST>& OldAST,
	const TSharedPtr<FAnimGraphAST>& NewAST)
{
	FAnimLangDiffResult Result;
	
	if (!OldAST.IsValid() && !NewAST.IsValid())
	{
		return Result;  // Both null = no changes
	}
	
	if (!OldAST.IsValid() || !NewAST.IsValid())
	{
		FAnimLangDiffEntry E;
		E.Op = EAnimLangDiffOp::RootChanged;
		E.OldValue = OldAST.IsValid() ? OldAST->Name : TEXT("(null)");
		E.NewValue = NewAST.IsValid() ? NewAST->Name : TEXT("(null)");
		Result.Entries.Add(E);
		return Result;
	}
	
	// Diff variables
	DiffVariables(OldAST->Variables, NewAST->Variables, Result.Entries);
	
	// Diff defines
	DiffDefines(OldAST->Defines, NewAST->Defines, Result.Entries);
	
	// Diff root node
	DiffNodes(OldAST->RootNode, NewAST->RootNode, TEXT("root"), Result.Entries);
	
	return Result;
}

// ========== Variables Diff ==========

void FAnimLangDiffer::DiffVariables(
	const TArray<FVariableDef>& OldVars,
	const TArray<FVariableDef>& NewVars,
	TArray<FAnimLangDiffEntry>& OutEntries)
{
	// Build name→index maps
	TMap<FString, int32> OldMap;
	for (int32 i = 0; i < OldVars.Num(); i++)
	{
		OldMap.Add(OldVars[i].Name, i);
	}
	
	TMap<FString, int32> NewMap;
	for (int32 i = 0; i < NewVars.Num(); i++)
	{
		NewMap.Add(NewVars[i].Name, i);
	}
	
	// Find removed
	for (const auto& Pair : OldMap)
	{
		if (!NewMap.Contains(Pair.Key))
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::VariableRemoved;
			E.VariableName = Pair.Key;
			OutEntries.Add(E);
		}
	}
	
	// Find added and changed
	for (const auto& Pair : NewMap)
	{
		const int32* OldIdx = OldMap.Find(Pair.Key);
		if (!OldIdx)
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::VariableAdded;
			E.VariableName = Pair.Key;
			OutEntries.Add(E);
		}
		else
		{
			const FVariableDef& OldVar = OldVars[*OldIdx];
			const FVariableDef& NewVar = NewVars[Pair.Value];
			
			if (OldVar.Type != NewVar.Type || OldVar.DefaultValue != NewVar.DefaultValue)
			{
				FAnimLangDiffEntry E;
				E.Op = EAnimLangDiffOp::VariableChanged;
				E.VariableName = Pair.Key;
				E.OldValue = OldVar.ToString();
				E.NewValue = NewVar.ToString();
				OutEntries.Add(E);
			}
		}
	}
}

// ========== Defines Diff ==========

void FAnimLangDiffer::DiffDefines(
	const TArray<FCachedPoseDef>& OldDefs,
	const TArray<FCachedPoseDef>& NewDefs,
	TArray<FAnimLangDiffEntry>& OutEntries)
{
	// Match by identifier
	TMap<FString, int32> OldMap;
	for (int32 i = 0; i < OldDefs.Num(); i++)
	{
		OldMap.Add(OldDefs[i].GetIdentifier(), i);
	}
	
	TMap<FString, int32> NewMap;
	for (int32 i = 0; i < NewDefs.Num(); i++)
	{
		NewMap.Add(NewDefs[i].GetIdentifier(), i);
	}
	
	// Removed
	for (const auto& Pair : OldMap)
	{
		if (!NewMap.Contains(Pair.Key))
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::DefineRemoved;
			E.ChildName = Pair.Key;
			OutEntries.Add(E);
		}
	}
	
	// Added and changed
	for (const auto& Pair : NewMap)
	{
		const int32* OldIdx = OldMap.Find(Pair.Key);
		if (!OldIdx)
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::DefineAdded;
			E.ChildName = Pair.Key;
			OutEntries.Add(E);
		}
		else
		{
			// Compare bodies
			const FCachedPoseDef& OldDef = OldDefs[*OldIdx];
			const FCachedPoseDef& NewDef = NewDefs[Pair.Value];
			
			FString OldBody = OldDef.Body.IsValid() ? OldDef.Body->ToString(0) : TEXT("");
			FString NewBody = NewDef.Body.IsValid() ? NewDef.Body->ToString(0) : TEXT("");
			
			if (OldBody != NewBody)
			{
				FAnimLangDiffEntry E;
				E.Op = EAnimLangDiffOp::DefineBodyChanged;
				E.ChildName = Pair.Key;
				OutEntries.Add(E);
			}
		}
	}
}

// ========== Nodes Diff (Recursive) ==========

void FAnimLangDiffer::DiffNodes(
	const TSharedPtr<FAnimNodeAST>& OldNode,
	const TSharedPtr<FAnimNodeAST>& NewNode,
	const FString& Path,
	TArray<FAnimLangDiffEntry>& OutEntries)
{
	// Handle null cases
	if (!OldNode.IsValid() && !NewNode.IsValid()) return;
	
	if (!OldNode.IsValid())
	{
		FAnimLangDiffEntry E;
		E.Op = EAnimLangDiffOp::NodeAdded;
		E.NodePath = Path;
		OutEntries.Add(E);
		return;
	}
	
	if (!NewNode.IsValid())
	{
		FAnimLangDiffEntry E;
		E.Op = EAnimLangDiffOp::NodeRemoved;
		E.NodePath = Path;
		OutEntries.Add(E);
		return;
	}
	
	// Check if node type changed
	if (OldNode->NodeType != NewNode->NodeType)
	{
		FAnimLangDiffEntry E;
		E.Op = EAnimLangDiffOp::RootChanged;
		E.NodePath = Path;
		E.OldValue = OldNode->NodeType;
		E.NewValue = NewNode->NodeType;
		OutEntries.Add(E);
		return;  // Don't recurse if type changed entirely
	}
	
	// Diff properties
	DiffProperties(OldNode->Properties, NewNode->Properties, Path, OutEntries);
	
	// Diff children
	DiffChildren(OldNode->Children, NewNode->Children, Path, OutEntries);
}

// ========== Properties Diff ==========

void FAnimLangDiffer::DiffProperties(
	const TMap<FString, FString>& OldProps,
	const TMap<FString, FString>& NewProps,
	const FString& Path,
	TArray<FAnimLangDiffEntry>& OutEntries)
{
	// Removed
	for (const auto& Pair : OldProps)
	{
		if (!NewProps.Contains(Pair.Key))
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::PropertyRemoved;
			E.NodePath = Path;
			E.PropertyKey = Pair.Key;
			E.OldValue = Pair.Value;
			OutEntries.Add(E);
		}
	}
	
	// Added and changed
	for (const auto& Pair : NewProps)
	{
		const FString* OldVal = OldProps.Find(Pair.Key);
		if (!OldVal)
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::PropertyAdded;
			E.NodePath = Path;
			E.PropertyKey = Pair.Key;
			E.NewValue = Pair.Value;
			OutEntries.Add(E);
		}
		else if (*OldVal != Pair.Value)
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::PropertyChanged;
			E.NodePath = Path;
			E.PropertyKey = Pair.Key;
			E.OldValue = *OldVal;
			E.NewValue = Pair.Value;
			OutEntries.Add(E);
		}
	}
}

// ========== Children Diff ==========

TArray<FAnimLangDiffer::FChildMatch> FAnimLangDiffer::MatchChildren(
	const TArray<FNamedChild>& OldChildren,
	const TArray<FNamedChild>& NewChildren)
{
	TArray<FChildMatch> Matches;
	
	TSet<int32> MatchedOld;
	TSet<int32> MatchedNew;
	
	// Pass 1: Match by pin name (most reliable)
	for (int32 NewIdx = 0; NewIdx < NewChildren.Num(); NewIdx++)
	{
		if (NewChildren[NewIdx].PinName.IsEmpty()) continue;
		
		for (int32 OldIdx = 0; OldIdx < OldChildren.Num(); OldIdx++)
		{
			if (MatchedOld.Contains(OldIdx)) continue;
			
			if (OldChildren[OldIdx].PinName == NewChildren[NewIdx].PinName)
			{
				FChildMatch M;
				M.OldIndex = OldIdx;
				M.NewIndex = NewIdx;
				M.bNameMatch = true;
				M.bTypeMatch = OldChildren[OldIdx].Node.IsValid() && NewChildren[NewIdx].Node.IsValid() &&
					OldChildren[OldIdx].Node->NodeType == NewChildren[NewIdx].Node->NodeType;
				Matches.Add(M);
				MatchedOld.Add(OldIdx);
				MatchedNew.Add(NewIdx);
				break;
			}
		}
	}
	
	// Pass 2: Match remaining by NodeId (if available)
	for (int32 NewIdx = 0; NewIdx < NewChildren.Num(); NewIdx++)
	{
		if (MatchedNew.Contains(NewIdx)) continue;
		if (!NewChildren[NewIdx].Node.IsValid() || NewChildren[NewIdx].Node->NodeId.IsEmpty()) continue;
		
		for (int32 OldIdx = 0; OldIdx < OldChildren.Num(); OldIdx++)
		{
			if (MatchedOld.Contains(OldIdx)) continue;
			if (!OldChildren[OldIdx].Node.IsValid()) continue;
			
			if (OldChildren[OldIdx].Node->NodeId == NewChildren[NewIdx].Node->NodeId)
			{
				FChildMatch M;
				M.OldIndex = OldIdx;
				M.NewIndex = NewIdx;
				M.bNameMatch = false;
				M.bTypeMatch = OldChildren[OldIdx].Node->NodeType == NewChildren[NewIdx].Node->NodeType;
				Matches.Add(M);
				MatchedOld.Add(OldIdx);
				MatchedNew.Add(NewIdx);
				break;
			}
		}
	}
	
	// Pass 3: Match remaining by position (unnamed children, same node type)
	for (int32 NewIdx = 0; NewIdx < NewChildren.Num(); NewIdx++)
	{
		if (MatchedNew.Contains(NewIdx)) continue;
		
		for (int32 OldIdx = 0; OldIdx < OldChildren.Num(); OldIdx++)
		{
			if (MatchedOld.Contains(OldIdx)) continue;
			
			// Same type → match
			bool bSameType = false;
			if (OldChildren[OldIdx].Node.IsValid() && NewChildren[NewIdx].Node.IsValid())
			{
				bSameType = OldChildren[OldIdx].Node->NodeType == NewChildren[NewIdx].Node->NodeType;
			}
			
			if (bSameType)
			{
				FChildMatch M;
				M.OldIndex = OldIdx;
				M.NewIndex = NewIdx;
				M.bNameMatch = false;
				M.bTypeMatch = true;
				Matches.Add(M);
				MatchedOld.Add(OldIdx);
				MatchedNew.Add(NewIdx);
				break;
			}
		}
	}
	
	return Matches;
}

void FAnimLangDiffer::DiffChildren(
	const TArray<FNamedChild>& OldChildren,
	const TArray<FNamedChild>& NewChildren,
	const FString& Path,
	TArray<FAnimLangDiffEntry>& OutEntries)
{
	TArray<FChildMatch> Matches = MatchChildren(OldChildren, NewChildren);
	
	TSet<int32> MatchedOld;
	TSet<int32> MatchedNew;
	for (const auto& M : Matches)
	{
		MatchedOld.Add(M.OldIndex);
		MatchedNew.Add(M.NewIndex);
	}
	
	// Removed children
	for (int32 i = 0; i < OldChildren.Num(); i++)
	{
		if (!MatchedOld.Contains(i))
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::ChildRemoved;
			E.NodePath = Path;
			E.ChildName = OldChildren[i].PinName.IsEmpty()
				? (OldChildren[i].Node.IsValid() ? OldChildren[i].Node->NodeType : TEXT("unknown"))
				: OldChildren[i].PinName;
			E.ChildIndex = i;
			OutEntries.Add(E);
		}
	}
	
	// Added children
	for (int32 i = 0; i < NewChildren.Num(); i++)
	{
		if (!MatchedNew.Contains(i))
		{
			FAnimLangDiffEntry E;
			E.Op = EAnimLangDiffOp::ChildAdded;
			E.NodePath = Path;
			E.ChildName = NewChildren[i].PinName.IsEmpty()
				? (NewChildren[i].Node.IsValid() ? NewChildren[i].Node->NodeType : TEXT("unknown"))
				: NewChildren[i].PinName;
			E.ChildIndex = i;
			OutEntries.Add(E);
		}
	}
	
	// Recursively diff matched children
	for (const auto& M : Matches)
	{
		const FNamedChild& OldChild = OldChildren[M.OldIndex];
		const FNamedChild& NewChild = NewChildren[M.NewIndex];
		
		FString ChildPath = Path + TEXT(".");
		if (!OldChild.PinName.IsEmpty())
		{
			ChildPath += OldChild.PinName;
		}
		else if (OldChild.Node.IsValid())
		{
			ChildPath += OldChild.Node->NodeType;
		}
		else
		{
			ChildPath += FString::Printf(TEXT("[%d]"), M.OldIndex);
		}
		
		DiffNodes(OldChild.Node, NewChild.Node, ChildPath, OutEntries);
	}
}
