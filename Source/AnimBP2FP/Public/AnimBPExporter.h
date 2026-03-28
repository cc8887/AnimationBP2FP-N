// AnimBPExporter.h - Export Animation Blueprint to DSL
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangAST.h"

// 注意：FAnimBPExporter 是编辑器专用功能，仅用于编辑器构建
// 非编辑器构建时，此类不可用

#if WITH_EDITOR

#include "Animation/AnimBlueprint.h"

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