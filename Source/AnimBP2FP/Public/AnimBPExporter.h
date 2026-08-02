// AnimBPExporter.h - Export Animation Blueprint to DSL
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangAST.h"
#include "RigLangExporter.h"

// 注意：FAnimBPExporter 是编辑器专用功能，仅用于编辑器构建
// 非编辑器构建时，此类不可用

#if WITH_EDITOR

#include "Animation/AnimBlueprint.h"

class UAnimGraphNode_Base;

/**
 * Exports UAnimBlueprint to AnimLang DSL code
 * 编辑器专用：仅用于编辑器构建
 */
class ANIMBP2FP_API FAnimBPExporter
{
public:
	/**
	 * Export an Animation Blueprint to DSL code
	 * @param AnimBlueprint The blueprint to export
	 * @return DSL code as string (S-expression format)
	 */
	static FString Export(UAnimBlueprint* AnimBlueprint);
	
	/**
	 * Export to AST (for programmatic manipulation)
	 * @param AnimBlueprint The blueprint to export
	 * @return AST representation
	 */
	static TSharedPtr<FAnimGraphAST> ExportToAST(UAnimBlueprint* AnimBlueprint);
	static TSharedPtr<FAnimGraphAST> ExportToAST(
		UAnimBlueprint* AnimBlueprint,
		TMap<FString, FRigLangExportResult>* OutRigModules);
	
	/**
	 * Export with options
	 */
	struct FExportOptions
	{
		bool bPrettyPrint = true;
		bool bIncludeComments = true;
		bool bOptimize = false;
		int32 IndentSize = 2;
	};
	
	static FString ExportWithOptions(UAnimBlueprint* AnimBlueprint, const FExportOptions& Options);
	static FString ExportWithOptions(
		UAnimBlueprint* AnimBlueprint,
		const FExportOptions& Options,
		TMap<FString, FRigLangExportResult>& OutRigModules,
		TSharedPtr<FAnimGraphAST>* OutAST = nullptr);

	// ---------------------------------------------------------------
	// EventGraph export via BlueprintLisp
	// ---------------------------------------------------------------

	struct FEventGraphExportOptions
	{
		FString GraphName         = TEXT("EventGraph");
		bool    bPrettyPrint      = true;
		bool    bIncludePositions = false;
		bool    bStableIds        = true;  // Emit :id tags for incremental update
	};

	/**
	 * Export the EventGraph (or any BP graph) of this AnimBlueprint to
	 * BlueprintLisp DSL using the BlueprintLisp plugin.
	 *
	 * @param AnimBlueprint   The AnimBlueprint whose EventGraph is exported
	 * @param Options         Export options
	 * @param OutLispCode     Receives the DSL text on success
	 * @param OutError        Receives the error message on failure
	 * @return true on success
	 */
	static bool ExportEventGraph(
		UAnimBlueprint*                AnimBlueprint,
		const FEventGraphExportOptions& Options,
		FString&                        OutLispCode,
		FString&                        OutError);

private:
	// Internal conversion methods
	static void TraverseAnimGraph(UEdGraph* Graph, TSharedPtr<FAnimGraphAST> OutAST);
	static TSharedPtr<FAnimNodeAST> ConvertAnimNode(UAnimGraphNode_Base* Node);
	static TSharedPtr<FStateMachineAST> ConvertStateMachine(class UAnimGraphNode_StateMachine* SMNode);
	static TSharedPtr<FExpressionAST> ConvertExpression(UEdGraphNode* ExprNode);
	
	// Pretty printer
	static FString ASTToString(const TSharedPtr<FAnimGraphAST>& AST, const FExportOptions& Options);
};

#endif // WITH_EDITOR
