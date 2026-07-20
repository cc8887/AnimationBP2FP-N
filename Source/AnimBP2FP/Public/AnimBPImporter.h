// AnimBPImporter.h - Import DSL code to Animation Blueprint
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangAST.h"
#include "AnimBP2FPModule.h"

// 注意：FAnimBPImporter 是编辑器专用功能，仅用于编辑器构建
// 非编辑器构建时，此类不可用

#if WITH_EDITOR

#include "Animation/AnimBlueprint.h"

class UAnimGraphNode_Base;
class UAnimGraphNode_StateMachine;
class UAnimGraphNode_SaveCachedPose;
class UEdGraph;
class UEdGraphPin;
class UEdGraphNode;

/**
 * Imports AnimLang DSL code to UAnimBlueprint
 * 编辑器专用：仅用于编辑器构建
 */
class ANIMBP2FP_API FAnimBPImporter
{
public:
	/**
	 * Import DSL code to create a new Animation Blueprint
	 * @param DSLCode The AnimLang code
	 * @param PackagePath Where to save the blueprint (e.g. "/Game/Animations/MyAnim_BP")
	 * @param OutError Error message if import fails
	 * @return The created blueprint, or nullptr on failure
	 */
	static UAnimBlueprint* Import(const FString& DSLCode, const FString& PackagePath, FString* OutError = nullptr);
	
	/**
	 * Import from AST
	 * @param AST The parsed AST
	 * @param PackagePath Where to save the blueprint
	 * @param OutError Error message if import fails
	 * @return The created blueprint, or nullptr on failure
	 */
	static UAnimBlueprint* ImportFromAST(const TSharedPtr<FAnimGraphAST>& AST, const FString& PackagePath, FString* OutError = nullptr);
	
	/**
	 * Update existing blueprint from DSL (incremental when possible, full rebuild as fallback)
	 * 
	 * Strategy:
	 *   1. Export current blueprint → old AST
	 *   2. Parse new DSL → new AST  
	 *   3. Diff old vs new
	 *   4a. If property-only changes: apply incremental patch (preserves node positions)
	 *   4b. If structural changes: clear AnimGraph + rebuild from new AST
	 *   5. Recompile
	 * 
	 * @param ExistingBlueprint The blueprint to update
	 * @param NewDSLCode The new DSL code
	 * @param OutError Error message if update fails
	 * @return true on success
	 */
	static bool UpdateBlueprint(UAnimBlueprint* ExistingBlueprint, const FString& NewDSLCode, FString* OutError = nullptr);
	
	/**
	 * Update result details (accessible after UpdateBlueprint)
	 */
	struct FUpdateResult
	{
		bool bSuccess = false;
		bool bUsedIncrementalPatch = false;  // true = incremental, false = full rebuild
		int32 NumChanges = 0;
		int32 NumPropertyChanges = 0;
		int32 NumStructuralChanges = 0;
		TArray<FString> AppliedOps;
		TArray<FString> Warnings;
		FString DiffSummary;
		
		FString ToString() const
		{
			FString Result;
			Result += FString::Printf(TEXT("Update %s (%s): %d changes (%d property, %d structural)\n"),
				bSuccess ? TEXT("succeeded") : TEXT("failed"),
				bUsedIncrementalPatch ? TEXT("incremental") : TEXT("full rebuild"),
				NumChanges, NumPropertyChanges, NumStructuralChanges);
			if (!DiffSummary.IsEmpty())
			{
				Result += TEXT("Diff: ") + DiffSummary + TEXT("\n");
			}
			for (const FString& Op : AppliedOps)
			{
				Result += TEXT("  ✓ ") + Op + TEXT("\n");
			}
			for (const FString& W : Warnings)
			{
				Result += TEXT("  ⚠ ") + W + TEXT("\n");
			}
			return Result;
		}
	};
	
	/**
	 * Update with detailed result reporting
	 */
	static FUpdateResult UpdateBlueprintDetailed(UAnimBlueprint* ExistingBlueprint, const FString& NewDSLCode);

private:
	// ========== Blueprint Creation ==========
	
	/** Create an empty Animation Blueprint with the given skeleton */
	static UAnimBlueprint* CreateEmptyBlueprint(const FString& PackagePath, const FString& BlueprintName, const FString& SkeletonPath);
	
	/** Find the AnimGraph (the root UEdGraph) inside the blueprint */
	static UEdGraph* FindAnimGraph(UAnimBlueprint* Blueprint);
	
	// ========== Graph Building ==========

	/** Build the entire animation graph from AST */
	static bool BuildAnimGraph(UAnimBlueprint* Blueprint, const TSharedPtr<FAnimGraphAST>& AST);

	/** Build variables from AST definitions */
	static bool BuildVariables(UAnimBlueprint* Blueprint, const TArray<FVariableDef>& Variables);

	/** Build generated bridge variables for helper graphs */
	static bool BuildGeneratedVars(UAnimBlueprint* Blueprint, const TArray<FHelperGraphDef>& Helpers);

	/** Build helper function graphs via BlueprintLisp import */
	static bool BuildHelperGraphs(UAnimBlueprint* Blueprint, const TArray<FHelperGraphDef>& Helpers);

	/** Restore ordinary EventGraph/function graphs via strict BlueprintLisp import. */
	static bool BuildLogicGraphs(UAnimBlueprint* Blueprint, const TArray<FLogicGraphDef>& LogicGraphs, bool bReplaceExistingSet);

	/** Build a single animation node from AST, placing it in the given graph */
	static UAnimGraphNode_Base* BuildAnimNode(const TSharedPtr<FAnimNodeAST>& NodeAST, UEdGraph* Graph,
		const TMap<FString, UAnimGraphNode_SaveCachedPose*>* DefineNodes = nullptr,
		const TMap<FString, FHelperGraphDef>* HelperGraphs = nullptr);

	/** Build a state machine node */
	static bool BuildStateMachine(UAnimGraphNode_StateMachine* SMNode, const TSharedPtr<FAnimNodeAST>& NodeAST,
		const TMap<FString, UAnimGraphNode_SaveCachedPose*>* DefineNodes = nullptr,
		const TMap<FString, FHelperGraphDef>* HelperGraphs = nullptr);


	// ========== Pin Utilities ==========

	/** Connect two pins (handles type and direction validation) */
	static void ConnectPins(UEdGraphPin* OutputPin, UEdGraphPin* InputPin);

	/** Find a pin by name on a node (direction-aware) */
	static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction);

	/** Find the output pose pin on a node */
	static UEdGraphPin* FindOutputPosePin(UAnimGraphNode_Base* Node);

	/** Find an input pose pin by its kebab-case DSL name */
	static UEdGraphPin* FindInputPosePin(UAnimGraphNode_Base* Node, const FString& KebabPinName);
	
	// ========== Name Conversion ==========
	
	/** Convert kebab-case DSL node type to UE class name (e.g. "sequence-player" -> "AnimGraphNode_SequencePlayer") */
	static FString KebabToCamelClassName(const FString& KebabName);
	
	/** Convert kebab-case to CamelCase (e.g. "blend-pose-0" -> "BlendPose0") */
	static FString KebabToCamel(const FString& Input);
	
	// ========== Node Factory ==========
	
	/** Try to find the UClass for a DSL node type string */
	static UClass* FindAnimNodeClass(const FString& DSLNodeType);
	
	/** Set a non-pose property on a created node */
	static bool SetNodeProperty(UAnimGraphNode_Base* Node, const FString& KebabKey, const FString& Value);

	/** Restore a non-pose binding such as bind-var / bind-path / subgraph-ref onto a pin or property binding */
	static bool ConnectPropertyBinding(UAnimBlueprint* Blueprint, UEdGraph* Graph, UAnimGraphNode_Base* Node,
		const FString& KebabKey, const FString& Value, const TMap<FString, FHelperGraphDef>* HelperGraphs = nullptr);


	
	// ========== Update Helpers ==========
	
	/** Clear the AnimGraph of all non-root nodes and connections */
	static void ClearAnimGraph(UEdGraph* AnimGraph);
	
	/** Clear the AnimGraph + rebuild entirely from new AST (preserves root node) */
	static bool RebuildAnimGraph(UAnimBlueprint* Blueprint, const TSharedPtr<FAnimGraphAST>& NewAST);
	
	/** Clear defines (SaveCachedPose nodes) and their subtrees */
	static void ClearDefines(UAnimBlueprint* Blueprint);
	
	// ========== Compilation ==========
	
	/** Compile the blueprint and check for errors */
	static bool CompileBlueprint(UAnimBlueprint* Blueprint, FString* OutError);

	static void BroadcastNodeLifecycle(
		AnimBP2FPImportLifecycle::EImportLifecyclePhase Phase,
		const AnimBP2FPImportLifecycle::FImportLifecycleContext& Context,
		const TArray<AnimBP2FPImportLifecycle::FImportNodeChange>& Changes);

	static void BroadcastPropertyLifecycle(
		AnimBP2FPImportLifecycle::EImportLifecyclePhase Phase,
		const AnimBP2FPImportLifecycle::FImportLifecycleContext& Context,
		const TArray<AnimBP2FPImportLifecycle::FImportPropertyChange>& Changes);

	static void BroadcastFinalizeLifecycle(
		AnimBP2FPImportLifecycle::EImportLifecyclePhase Phase,
		const AnimBP2FPImportLifecycle::FImportLifecycleContext& Context);
};

#endif // WITH_EDITOR
