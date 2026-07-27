// AnimLangPatcher.h - Incremental Blueprint Patcher
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangAST.h"
#include "AnimLangDiffer.h"

#if WITH_EDITOR

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"

/**
 * Patch result
 */
struct ANIMBP2FP_API FAnimLangPatchResult
{
	bool bSuccess;
	TArray<FString> AppliedOps;    // Successfully applied operations
	TArray<FString> FailedOps;     // Operations that could not be applied
	TArray<FString> Warnings;      // Non-fatal warnings
	
	FString ToString() const;
};

/**
 * AnimLang Incremental Patcher
 * 
 * Applies a diff to an existing UAnimBlueprint without recreating it from scratch.
 * This preserves:
 *   - Node positions in the editor graph
 *   - Connections not affected by the diff
 *   - Custom user data and breakpoints
 *   - Undo history (wraps in a single transaction)
 * 
 * Workflow:
 *   1. Export current blueprint to AST (old)
 *   2. Parse edited DSL to AST (new)
 *   3. Compute diff: Differ::Diff(old, new)
 *   4. Apply diff: Patcher::Apply(blueprint, diff)
 *   5. Recompile blueprint
 */
class ANIMBP2FP_API FAnimLangPatcher
{
public:
	/**
	 * Apply a diff to an existing blueprint
	 * @param Blueprint  The blueprint to modify
	 * @param Diff  The diff to apply
	 * @param NewAST  The complete new AST (needed for add operations)
	 * @return Patch result with details
	 */
	static FAnimLangPatchResult Apply(
		UAnimBlueprint* Blueprint,
		const FAnimLangDiffResult& Diff,
		const TSharedPtr<FAnimGraphAST>& NewAST);
	
	/**
	 * Convenience: Full incremental update pipeline
	 * Export → Parse → Diff → Patch → Compile
	 * @param Blueprint  The blueprint to update
	 * @param NewDSLCode  The new DSL code
	 * @return Patch result
	 */
	static FAnimLangPatchResult IncrementalUpdate(
		UAnimBlueprint* Blueprint,
		const FString& NewDSLCode);

private:
	// Apply individual diff operations
	static bool ApplyPropertyChange(UAnimBlueprint* Blueprint, const FAnimLangDiffEntry& Entry, TArray<FString>& OutWarnings);
	static bool ApplyVariableChange(UAnimBlueprint* Blueprint, const FAnimLangDiffEntry& Entry, const TSharedPtr<FAnimGraphAST>& NewAST, TArray<FString>& OutWarnings);
	static bool ApplyMapVariableDefaults(
		UAnimBlueprint* Blueprint,
		const TArray<FVariableDef>& Variables,
		FString& OutError);
	static bool ApplyNodeStructuralChange(UAnimBlueprint* Blueprint, const FAnimLangDiffEntry& Entry, const TSharedPtr<FAnimGraphAST>& NewAST, TArray<FString>& OutWarnings);
	
	// Find an animation graph node by path
	static UAnimGraphNode_Base* FindNodeByPath(UEdGraph* AnimGraph, const FString& NodePath);
	
	// Set a property value on a node
	static bool SetNodeProperty(UAnimGraphNode_Base* Node, const FString& PropertyKey, const FString& Value);
	
	// Blueprint compilation
	static bool CompileBlueprint(UAnimBlueprint* Blueprint, FString& OutError);
};

#endif // WITH_EDITOR
