// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "BlueprintLispConverter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPMoverExternalVariableSetRoundTrips,
	"AnimBP2FP.Mover.ExternalVariableSetRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPMoverExternalVariableSetRoundTrips::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(
		nullptr, TEXT("/Game/Blueprints/SandboxCharacter_Mover_ABP.SandboxCharacter_Mover_ABP"));
	TestNotNull(TEXT("real Mover AnimBlueprint loads"), Source);
	if (!Source) return false;

	UEdGraph* SourceGraph = nullptr;
	for (UEdGraph* Graph : Source->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == TEXT("InitializeMoverPredictor"))
		{
			SourceGraph = Graph;
			break;
		}
	}
	TestNotNull(TEXT("InitializeMoverPredictor graph exists"), SourceGraph);
	if (!SourceGraph) return false;

	FBlueprintLispConverter::FExportOptions ExportOptions;
	ExportOptions.bPrettyPrint = false;
	ExportOptions.bStableIds = true;
	const FBlueprintLispResult SourceExport = FBlueprintLispConverter::ExportGraph(SourceGraph, ExportOptions);
	TestTrue(TEXT("InitializeMoverPredictor exports"), SourceExport.bSuccess);
	TestTrue(TEXT("external property set preserves owner"),
		SourceExport.LispCode.Contains(TEXT(":owner \"/Script/Mover.MoverTrajectoryPredictor\"")));
	TestTrue(TEXT("external property set preserves target object"),
		SourceExport.LispCode.Contains(TEXT(":self Predictor"))
		|| SourceExport.LispCode.Contains(TEXT(":self (self.Predictor)")));
	TestTrue(TEXT("FrameRate value is exported structurally"),
		SourceExport.LispCode.Contains(TEXT("make-struct"))
		&& SourceExport.LispCode.Contains(TEXT("/Script/CoreUObject.FrameRate")));

	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_MoverExternalSetRoundTrip"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPMoverVariableSetTest"))));
	TestNotNull(TEXT("transient destination AnimBlueprint is created"), Destination);
	if (!Destination) return false;

	UClass* PredictorClass = LoadObject<UClass>(nullptr, TEXT("/Script/Mover.MoverTrajectoryPredictor"));
	TestNotNull(TEXT("MoverTrajectoryPredictor class loads"), PredictorClass);
	if (!PredictorClass) return false;
	FEdGraphPinType PredictorType;
	PredictorType.PinCategory = UEdGraphSchema_K2::PC_Object;
	PredictorType.PinSubCategoryObject = PredictorClass;
	TestTrue(TEXT("Predictor fixture variable is created"),
		FBlueprintEditorUtils::AddMemberVariable(Destination, TEXT("Predictor"), PredictorType));

	UEdGraph* DestinationGraph = FBlueprintEditorUtils::CreateNewGraph(
		Destination, TEXT("TestExternalSet"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	TestNotNull(TEXT("fixture function graph is created"), DestinationGraph);
	if (!DestinationGraph) return false;
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Destination, DestinationGraph, true, nullptr);

	const FString FixtureDSL =
		TEXT("(function TestExternalSet :thread-safe false ")
		TEXT("(set MoverSamplingFrameRate ")
		TEXT("(make-struct :struct \"/Script/CoreUObject.FrameRate\" ")
		TEXT(":field (Numerator int 60) :field (Denominator int 1)) ")
		TEXT(":owner \"/Script/Mover.MoverTrajectoryPredictor\" :self (self.Predictor)))");
	FBlueprintLispConverter::FImportOptions ImportOptions;
	ImportOptions.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
	ImportOptions.bAutoLayout = false;
	ImportOptions.bCompile = false;
	ImportOptions.bFailOnUnsupportedForm = true;
	const FBlueprintLispResult ImportResult =
		FBlueprintLispConverter::ImportGraph(DestinationGraph, FixtureDSL, ImportOptions);
	TestTrue(TEXT("external property set imports"), ImportResult.bSuccess);
	if (!ImportResult.bSuccess)
	{
		AddError(ImportResult.Error);
		return false;
	}

	UK2Node_VariableSet* ImportedSet = nullptr;
	for (UEdGraphNode* Node : DestinationGraph->Nodes)
	{
		if (UK2Node_VariableSet* VariableSet = Cast<UK2Node_VariableSet>(Node);
			VariableSet && VariableSet->VariableReference.GetMemberName() == TEXT("MoverSamplingFrameRate"))
		{
			ImportedSet = VariableSet;
			break;
		}
	}
	TestNotNull(TEXT("external VariableSet node is created"), ImportedSet);
	if (!ImportedSet) return false;
	TestEqual(TEXT("VariableSet owner class is restored"),
		ImportedSet->VariableReference.GetMemberParentClass(), PredictorClass);
	UEdGraphPin* SelfPin = ImportedSet->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
	TestNotNull(TEXT("external VariableSet has a self pin"), SelfPin);
	TestTrue(TEXT("external VariableSet self pin is connected"), SelfPin && SelfPin->LinkedTo.Num() == 1);
	UEdGraphPin* ValuePin = ImportedSet->FindPin(TEXT("MoverSamplingFrameRate"), EGPD_Input);
	TestNotNull(TEXT("FrameRate value pin exists"), ValuePin);
	TestTrue(TEXT("FrameRate value is connected from a make-struct node"),
		ValuePin && ValuePin->LinkedTo.Num() == 1);
	return true;
}

#endif
