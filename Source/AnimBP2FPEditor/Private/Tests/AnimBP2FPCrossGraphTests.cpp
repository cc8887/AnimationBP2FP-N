// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"

#include "AnimBPExporter.h"
#include "Animation/AnimBlueprint.h"
#include "BlueprintLispConverter.h"
#include "EdGraph/EdGraph.h"

#if WITH_DEV_AUTOMATION_TESTS && ANIMBP2FP_HAS_ANIM_AUTHORING

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPLinkedPureExpressionsAreStructured,
	"AnimBP2FP.CrossGraph.LinkedPureExpressionsAreStructured",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPLinkedPureExpressionsAreStructured::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Blueprints/SandboxCharacter_CMC_ABP.SandboxCharacter_CMC_ABP"));
	if (!Source)
	{
		AddInfo(TEXT("SKIPPED: real CMC AnimBlueprint fixture is not installed"));
		return true;
	}

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Source);
	TestTrue(TEXT("CMC exports"), AST.IsValid());
	if (!AST.IsValid()) return false;

	const FString DSL = AST->ToString();
	TestFalse(TEXT("linked Contains Item expression is not reduced to a node title"),
		DSL.Contains(TEXT("(var \"Contains Item\")")));
	TestFalse(TEXT("linked desired-facing expression is not reduced to a node title"),
		DSL.Contains(TEXT("(var \"Get_DesiredFacing\")")));
	TestFalse(TEXT("linked blend-stack asset expression is not reduced to a node title"),
		DSL.Contains(TEXT("(var \"GetCurrentBlendStackAnimAsset\")")));
	TestTrue(TEXT("linked pure expressions use the structured value form"),
		DSL.Contains(TEXT("(value-expr ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPPropertyAccessSplitStructOutputsAreStructured,
	"AnimBP2FP.CrossGraph.PropertyAccessSplitStructOutputsAreStructured",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPPropertyAccessSplitStructOutputsAreStructured::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Blueprints/SandboxCharacter_CMC_ABP.SandboxCharacter_CMC_ABP"));
	if (!Source)
	{
		AddInfo(TEXT("SKIPPED: real CMC AnimBlueprint fixture is not installed"));
		return true;
	}

	UEdGraph* TargetRotationGraph = nullptr;
	for (UEdGraph* Graph : Source->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == TEXT("Update_TargetRotation"))
		{
			TargetRotationGraph = Graph;
			break;
		}
	}
	TestNotNull(TEXT("Update_TargetRotation graph exists"), TargetRotationGraph);
	if (!TargetRotationGraph) return false;

	FBlueprintLispConverter::FExportOptions Options;
	Options.bPrettyPrint = false;
	Options.bStableIds = true;
	const FBlueprintLispResult Exported = FBlueprintLispConverter::ExportGraph(TargetRotationGraph, Options);
	TestTrue(TEXT("Update_TargetRotation exports"), Exported.bSuccess);
	TestTrue(TEXT("property-access Roll output is represented as a break-struct field"),
		Exported.LispCode.Contains(TEXT(":field Roll")));
	TestTrue(TEXT("property-access Pitch output is represented as a break-struct field"),
		Exported.LispCode.Contains(TEXT(":field Pitch")));
	TestTrue(TEXT("property-access Yaw output is represented as a break-struct field"),
		Exported.LispCode.Contains(TEXT(":field Yaw")));
	return true;
}

#endif
