// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "AnimLangParser.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AnimBP2FPLogicGraphTests
{
	static UEdGraph* AddGraph(UAnimBlueprint* Blueprint, const FName GraphName, const bool bFunction)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, GraphName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (bFunction)
		{
			FBlueprintEditorUtils::AddFunctionGraph(Blueprint, Graph, false, static_cast<UFunction*>(nullptr));
		}
		else
		{
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
		}
		return Graph;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPMainDSLIncludesBlueprintLispLogicGraphs,
	"AnimBP2FP.LogicGraphs.MainDSLIncludesBlueprintLisp",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPMainDSLIncludesBlueprintLispLogicGraphs::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPLogicGraphTests;

	UAnimBlueprint* Blueprint = NewObject<UAnimBlueprint>(GetTransientPackage(), TEXT("ABP_LogicGraphFixture"));

	UEdGraph* EventGraph = AddGraph(Blueprint, TEXT("EventGraph"), false);
	UK2Node_CustomEvent* CustomEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
	CustomEvent->CustomFunctionName = TEXT("OnLogicFixture");
	CustomEvent->CreateNewGuid();
	CustomEvent->AllocateDefaultPins();
	EventGraph->AddNode(CustomEvent, false, false);

	UEdGraph* FunctionGraph = AddGraph(Blueprint, TEXT("ComputeLogicFixture"), true);
	UK2Node_FunctionEntry* FunctionEntry = NewObject<UK2Node_FunctionEntry>(FunctionGraph);
	FunctionEntry->CreateNewGuid();
	FunctionEntry->AllocateDefaultPins();
	FunctionGraph->AddNode(FunctionEntry, false, false);

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("AST exports both logic graphs"), AST.IsValid() && AST->LogicGraphs.Num() == 2);
	if (!AST.IsValid() || AST->LogicGraphs.Num() != 2)
	{
		return false;
	}

	TestEqual(TEXT("event graph is first in stable order"), AST->LogicGraphs[0].GraphName, FString(TEXT("EventGraph")));
	TestEqual(TEXT("event role is preserved"), AST->LogicGraphs[0].Role, FString(TEXT("event")));
	TestEqual(TEXT("function graph is second"), AST->LogicGraphs[1].GraphName, FString(TEXT("ComputeLogicFixture")));
	TestEqual(TEXT("function role is preserved"), AST->LogicGraphs[1].Role, FString(TEXT("function")));
	TestTrue(TEXT("event DSL contains custom event"), AST->LogicGraphs[0].DSL.Contains(TEXT("OnLogicFixture")));
	TestTrue(TEXT("function DSL contains function form"), AST->LogicGraphs[1].DSL.Contains(TEXT("function")));

	const FString DSL = AST->ToString();
	TestTrue(TEXT("main DSL emits logic graph block"), DSL.Contains(TEXT("(logic-graphs")));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("main DSL with BlueprintLisp parses"), Parsed.IsValid() && Errors.Num() == 0);
	if (!Parsed.IsValid() || Parsed->LogicGraphs.Num() != 2)
	{
		return false;
	}

	const FString Reserialized = Parsed->ToString();
	TestTrue(TEXT("event BlueprintLisp survives parse/serialize"), Reserialized.Contains(TEXT("OnLogicFixture")));
	TestTrue(TEXT("function BlueprintLisp survives parse/serialize"), Reserialized.Contains(TEXT("ComputeLogicFixture")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPImporterRestoresLogicGraphs,
	"AnimBP2FP.LogicGraphs.ImporterRestoresBeforeCompile",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPImporterRestoresLogicGraphs::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPLogicGraphTests;

	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_LogicGraphImportFixture");

	FLogicGraphDef EventGraphDef;
	EventGraphDef.Role = TEXT("event");
	EventGraphDef.Kind = TEXT("ubergraph");
	EventGraphDef.GraphName = TEXT("EventGraph");
	EventGraphDef.DSL = TEXT("(event custom OnImportedLogic)");
	AST->LogicGraphs.Add(EventGraphDef);

	FLogicGraphDef FunctionGraphDef;
	FunctionGraphDef.Role = TEXT("function");
	FunctionGraphDef.Kind = TEXT("function");
	FunctionGraphDef.GraphName = TEXT("ImportedLogicFunction");
	FunctionGraphDef.DSL = TEXT("(function ImportedLogicFunction)");
	AST->LogicGraphs.Add(FunctionGraphDef);

	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(),
		GetTransientPackage(),
		TEXT("ABP_LogicGraphDestination"),
		BPTYPE_Normal,
		UAnimBlueprint::StaticClass(),
		UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPLogicGraphTest"))));
	TestNotNull(TEXT("valid destination AnimBlueprint is created"), Destination);
	if (!Destination)
	{
		return false;
	}
	const FAnimBPImporter::FUpdateResult Result = FAnimBPImporter::UpdateBlueprintDetailed(Destination, AST->ToString());
	TestTrue(TEXT("logic graph rebuild succeeds"), Result.bSuccess);

	UEdGraph* RestoredEventGraph = nullptr;
	for (UEdGraph* Graph : Destination->UbergraphPages)
	{
		if (Graph && Graph->GetName() == TEXT("EventGraph"))
		{
			RestoredEventGraph = Graph;
			break;
		}
	}
	TestTrue(TEXT("EventGraph is created"), RestoredEventGraph != nullptr);
	TestTrue(TEXT("EventGraph nodes are restored"), RestoredEventGraph && RestoredEventGraph->Nodes.Num() != 0);

	UEdGraph* RestoredFunctionGraph = nullptr;
	for (UEdGraph* Graph : Destination->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TEXT("ImportedLogicFunction"))
		{
			RestoredFunctionGraph = Graph;
			break;
		}
	}
	TestTrue(TEXT("function graph is created"), RestoredFunctionGraph != nullptr);
	TestTrue(TEXT("function graph nodes are restored"), RestoredFunctionGraph && RestoredFunctionGraph->Nodes.Num() != 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPPureFunctionFlagsRoundTrip,
	"AnimBP2FP.LogicGraphs.PureFunctionFlagsRoundTrip",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPPureFunctionFlagsRoundTrip::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPLogicGraphTests;

	UAnimBlueprint* Source = NewObject<UAnimBlueprint>(GetTransientPackage(), TEXT("ABP_PureFunctionSource"));
	UEdGraph* SourceGraph = AddGraph(Source, TEXT("ComputePureValue"), true);
	UK2Node_FunctionEntry* SourceEntry = nullptr;
	for (UEdGraphNode* Node : SourceGraph->Nodes)
	{
		if ((SourceEntry = Cast<UK2Node_FunctionEntry>(Node))) break;
	}
	TestNotNull(TEXT("source function entry exists"), SourceEntry);
	if (!SourceEntry) return false;
	SourceEntry->SetExtraFlags(SourceEntry->GetExtraFlags() | FUNC_BlueprintPure);

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Source);
	TestTrue(TEXT("pure function source exports"), AST.IsValid());
	if (!AST.IsValid()) return false;
	const FLogicGraphDef* PureGraphDef = AST->LogicGraphs.FindByPredicate(
		[](const FLogicGraphDef& Def) { return Def.GraphName == TEXT("ComputePureValue"); });
	TestNotNull(TEXT("pure function logic graph exports"), PureGraphDef);
	if (!PureGraphDef) return false;
	TestTrue(TEXT("function DSL records pure flag"), PureGraphDef->DSL.Contains(TEXT(":pure true")));

	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_PureFunctionDestination"),
		BPTYPE_Normal, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPPureFunctionFlagsTest"))));
	TestNotNull(TEXT("pure function destination is created"), Destination);
	if (!Destination) return false;

	const FAnimBPImporter::FUpdateResult Result = FAnimBPImporter::UpdateBlueprintDetailed(Destination, AST->ToString());
	TestTrue(TEXT("pure function imports and compiles"), Result.bSuccess);

	UEdGraph* DestinationGraph = nullptr;
	for (UEdGraph* Graph : Destination->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TEXT("ComputePureValue"))
		{
			DestinationGraph = Graph;
			break;
		}
	}
	TestNotNull(TEXT("destination pure function graph exists"), DestinationGraph);
	UK2Node_FunctionEntry* DestinationEntry = nullptr;
	if (DestinationGraph)
	{
		for (UEdGraphNode* Node : DestinationGraph->Nodes)
		{
			if ((DestinationEntry = Cast<UK2Node_FunctionEntry>(Node))) break;
		}
	}
	TestTrue(TEXT("destination entry preserves BlueprintPure"), DestinationEntry
		&& (DestinationEntry->GetFunctionFlags() & FUNC_BlueprintPure) != 0);

	UFunction* GeneratedFunction = Destination->SkeletonGeneratedClass
		? Destination->SkeletonGeneratedClass->FindFunctionByName(TEXT("ComputePureValue")) : nullptr;
	TestTrue(TEXT("skeleton function preserves BlueprintPure"), GeneratedFunction
		&& GeneratedFunction->HasAnyFunctionFlags(FUNC_BlueprintPure));
	return true;
}

#endif
#endif // UE 5.8+