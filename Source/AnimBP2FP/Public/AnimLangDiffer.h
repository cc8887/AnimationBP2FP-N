// AnimLangDiffer.h - AST Diff Engine for Incremental Updates
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangAST.h"

/**
 * Types of diff operations
 */
enum class EAnimLangDiffOp : uint8
{
	NodeAdded,        // A new node was added
	NodeRemoved,      // A node was removed
	NodeMoved,        // A node was moved to a different parent/position
	PropertyChanged,  // A property value changed
	PropertyAdded,    // A new property was added
	PropertyRemoved,  // A property was removed
	ChildAdded,       // A new child node was added
	ChildRemoved,     // A child node was removed
	ChildReordered,   // Children were reordered
	DefineAdded,      // A new (define ...) was added
	DefineRemoved,    // A (define ...) was removed
	DefineBodyChanged,// A define's body changed
	VariableAdded,    // A new variable was added
	VariableRemoved,  // A variable was removed
	VariableChanged,  // A variable's type or default changed
	RootChanged,      // The root node changed type entirely
};

/**
 * A single diff entry describing one change between two ASTs
 */
struct ANIMBP2FP_API FAnimLangDiffEntry
{
	EAnimLangDiffOp Op;
	
	// Path to the affected node (dot-separated, e.g. "root.blend-pose-0.sequence-player")
	FString NodePath;
	
	// For property changes
	FString PropertyKey;
	FString OldValue;
	FString NewValue;
	
	// For child/define changes
	FString ChildName;       // Named child pin or define name
	int32 ChildIndex = -1;   // Index in children array
	
	// For variable changes
	FString VariableName;
	
	/** Human-readable description */
	FString ToString() const;
	
	/** Severity (0 = cosmetic, 1 = structural, 2 = breaking) */
	int32 GetSeverity() const;
};

/**
 * Complete diff result between two ASTs
 */
struct ANIMBP2FP_API FAnimLangDiffResult
{
	TArray<FAnimLangDiffEntry> Entries;
	
	bool HasChanges() const { return Entries.Num() > 0; }
	int32 NumStructuralChanges() const;
	int32 NumPropertyChanges() const;
	
	FString ToSummary() const;
	FString ToDetailedReport() const;
};

/**
 * AnimLang AST Differ
 * 
 * Compares two ASTs (Old vs New) and produces a minimal set of diff operations.
 * Uses NodeId for stable identity matching when available.
 * Falls back to positional matching + node type comparison.
 */
class ANIMBP2FP_API FAnimLangDiffer
{
public:
	/**
	 * Compute diff between two ASTs
	 * @param OldAST  The previous version (e.g., current state of the blueprint)
	 * @param NewAST  The new version (e.g., user-edited DSL)
	 * @return Diff result with all changes
	 */
	static FAnimLangDiffResult Diff(
		const TSharedPtr<FAnimGraphAST>& OldAST,
		const TSharedPtr<FAnimGraphAST>& NewAST);

private:
	// Compare variables
	static void DiffVariables(
		const TArray<FVariableDef>& OldVars,
		const TArray<FVariableDef>& NewVars,
		TArray<FAnimLangDiffEntry>& OutEntries);
	
	// Compare defines
	static void DiffDefines(
		const TArray<FCachedPoseDef>& OldDefs,
		const TArray<FCachedPoseDef>& NewDefs,
		TArray<FAnimLangDiffEntry>& OutEntries);
	
	// Compare node subtrees recursively
	static void DiffNodes(
		const TSharedPtr<FAnimNodeAST>& OldNode,
		const TSharedPtr<FAnimNodeAST>& NewNode,
		const FString& Path,
		TArray<FAnimLangDiffEntry>& OutEntries);
	
	// Compare properties maps
	static void DiffProperties(
		const TMap<FString, FString>& OldProps,
		const TMap<FString, FString>& NewProps,
		const FString& Path,
		TArray<FAnimLangDiffEntry>& OutEntries);
	
	// Compare named children
	static void DiffChildren(
		const TArray<FNamedChild>& OldChildren,
		const TArray<FNamedChild>& NewChildren,
		const FString& Path,
		TArray<FAnimLangDiffEntry>& OutEntries);
	
	// Match children by name and/or NodeId
	struct FChildMatch
	{
		int32 OldIndex;
		int32 NewIndex;
		bool bNameMatch;   // Matched by pin name
		bool bTypeMatch;   // Same node type
	};
	
	static TArray<FChildMatch> MatchChildren(
		const TArray<FNamedChild>& OldChildren,
		const TArray<FNamedChild>& NewChildren);
};
