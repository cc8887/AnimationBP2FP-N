// AnimBPImporter.h - Import DSL code to Animation Blueprint
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimBlueprint.h"
#include "AnimLangAST.h"

/**
 * Imports AnimLang DSL code to UAnimBlueprint
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
	 * Update existing blueprint from DSL (for live editing)
	 * @param ExistingBlueprint The blueprint to update
	 * @param DSLCode The new DSL code
	 * @param OutError Error message if update fails
	 * @return true on success
	 */
	static bool UpdateBlueprint(UAnimBlueprint* ExistingBlueprint, const FString& DSLCode, FString* OutError = nullptr);

private:
	// Internal construction methods
	static UAnimBlueprint* CreateEmptyBlueprint(const FString& PackagePath, const FString& BlueprintName);
	static bool BuildAnimGraph(UAnimBlueprint* Blueprint, const TSharedPtr<FAnimGraphAST>& AST);
	static UAnimGraphNode_Base* BuildAnimNode(const TSharedPtr<FAnimNodeAST>& NodeAST, UEdGraph* Graph);
	static bool BuildStateMachine(class UAnimGraphNode_StateMachine* SMNode, const TSharedPtr<FStateMachineAST>& SMAST);
	static void ConnectPins(UEdGraphPin* OutputPin, UEdGraphPin* InputPin);
	
	// Compilation
	static bool CompileBlueprint(UAnimBlueprint* Blueprint, FString* OutError);
};