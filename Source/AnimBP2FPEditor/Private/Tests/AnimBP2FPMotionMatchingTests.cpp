// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString FindMotionMatchingCallbackLine(const FString& DSL)
	{
		TArray<FString> Lines;
		DSL.ParseIntoArrayLines(Lines, false);
		for (FString Line : Lines)
		{
			if (Line.Contains(TEXT(":on-motion-matching-state-updated-function-ref")))
			{
				return Line.TrimStartAndEnd();
			}
		}
		return FString();
	}

	FString DescribeFunctionSignature(const UFunction* Function)
	{
		if (!Function) return TEXT("<null>");
		FString Description = FString::Printf(TEXT("flags=%llu parms=%d size=%d"),
			static_cast<uint64>(Function->FunctionFlags), Function->NumParms, Function->ParmsSize);
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
			Description += FString::Printf(TEXT(" | %s:%s:%s:flags=%llu"),
				*Property->GetName(), *Property->GetClass()->GetName(), *Property->GetCPPType(),
				static_cast<uint64>(Property->GetPropertyFlags()));
		}
		return Description;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPMotionMatchingCallbackRoundTrips,
	"AnimBP2FP.MotionMatching.CallbackRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPMotionMatchingCallbackRoundTrips::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(
		nullptr, TEXT("/Game/Blueprints/SandboxCharacter_CMC_ABP.SandboxCharacter_CMC_ABP"));
	TestNotNull(TEXT("real CMC AnimBlueprint loads"), Source);
	if (!Source) return false;

	const TSharedPtr<FAnimGraphAST> SourceAST = FAnimBPExporter::ExportToAST(Source);
	TestTrue(TEXT("real CMC AnimBlueprint exports"), SourceAST.IsValid());
	if (!SourceAST.IsValid()) return false;

	const FString SourceDSL = SourceAST->ToString();
	TestTrue(TEXT("source contains the Motion Matching callback"),
		SourceDSL.Contains(TEXT("on-motion-matching-state-updated-function-ref"))
		&& SourceDSL.Contains(TEXT("Update_MotionMatching_PostSelection")));
	TestTrue(TEXT("source callback uses structured member-ref DSL"),
		FindMotionMatchingCallbackLine(SourceDSL).Contains(
			TEXT("(member-ref :name \"Update_MotionMatching_PostSelection\" :self true)")));
	TestFalse(TEXT("source callback DSL does not contain a source-specific function GUID"),
		FindMotionMatchingCallbackLine(SourceDSL).Contains(TEXT("24796C9B4A2D043B3F7961BDC96B21A8")));

	UClass* ParentClass = Source->ParentClass ? Source->ParentClass.Get() : UAnimInstance::StaticClass();
	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		ParentClass, GetTransientPackage(), TEXT("ABP_MotionMatchingCallbackRoundTrip"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPMotionMatchingTest"))));
	TestNotNull(TEXT("transient destination AnimBlueprint is created"), Destination);
	if (!Destination) return false;
	Destination->TargetSkeleton = Source->TargetSkeleton;

	const FAnimBPImporter::FUpdateResult ImportResult =
		FAnimBPImporter::UpdateBlueprintDetailed(Destination, SourceDSL);
	TestTrue(TEXT("real CMC DSL import succeeds"), ImportResult.bSuccess);

	UEdGraph* CallbackGraph = nullptr;
	for (UEdGraph* FunctionGraph : Destination->FunctionGraphs)
	{
		if (FunctionGraph && FunctionGraph->GetFName() == TEXT("Update_MotionMatching_PostSelection"))
		{
			CallbackGraph = FunctionGraph;
			break;
		}
	}
	TestNotNull(TEXT("callback function graph exists after import"), CallbackGraph);
	UFunction* SourceFunction = Source->SkeletonGeneratedClass
		? Source->SkeletonGeneratedClass->FindFunctionByName(TEXT("Update_MotionMatching_PostSelection"))
		: nullptr;
	UFunction* DestinationFunction = Destination->SkeletonGeneratedClass
		? Destination->SkeletonGeneratedClass->FindFunctionByName(TEXT("Update_MotionMatching_PostSelection"))
		: nullptr;
	TestNotNull(TEXT("source callback UFunction exists"), SourceFunction);
	TestNotNull(TEXT("destination callback UFunction exists in SkeletonGeneratedClass"), DestinationFunction);
	AddInfo(TEXT("Source callback signature: ") + DescribeFunctionSignature(SourceFunction));
	AddInfo(TEXT("Destination callback signature: ") + DescribeFunctionSignature(DestinationFunction));
	if (SourceFunction && DestinationFunction)
	{
		TestTrue(TEXT("destination callback signature matches source"),
			SourceFunction->IsSignatureCompatibleWith(DestinationFunction));
	}
	if (const FLogicGraphDef* CallbackLogic = SourceAST->LogicGraphs.FindByPredicate([](const FLogicGraphDef& Logic)
		{ return Logic.GraphName == TEXT("Update_MotionMatching_PostSelection"); }))
	{
		AddInfo(TEXT("Source callback DSL: ") + CallbackLogic->DSL);
		TestTrue(TEXT("Context parameter preserves const-ref qualifiers"),
			CallbackLogic->DSL.Contains(TEXT(":param (Context animupdatecontext :ref true :const true)")));
		TestTrue(TEXT("Node parameter preserves const-ref qualifiers"),
			CallbackLogic->DSL.Contains(TEXT(":param (Node animnodereference :ref true :const true)")));
	}

	UEdGraphNode* MotionMatchingNode = nullptr;
	TArray<UEdGraph*> Graphs;
	Destination->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		if (TObjectPtr<UEdGraphNode>* Found = Graph->Nodes.FindByPredicate([](const UEdGraphNode* Node)
		{
			return Node && Node->GetClass()->GetName() == TEXT("AnimGraphNode_MotionMatching");
		}))
		{
			MotionMatchingNode = *Found;
		}
		if (MotionMatchingNode) break;
	}
	TestNotNull(TEXT("imported Motion Matching node exists"), MotionMatchingNode);
	if (!MotionMatchingNode) return false;

	FProperty* CallbackProperty = MotionMatchingNode->GetClass()->FindPropertyByName(
		TEXT("OnMotionMatchingStateUpdatedFunction"));
	TestNotNull(TEXT("Motion Matching callback property exists"), CallbackProperty);
	if (!CallbackProperty) return false;

	FString ImportedReference;
	CallbackProperty->ExportText_Direct(
		ImportedReference,
		CallbackProperty->ContainerPtrToValuePtr<void>(MotionMatchingNode),
		nullptr,
		MotionMatchingNode,
		PPF_None);
	TestTrue(TEXT("callback member name is restored"),
		ImportedReference.Contains(TEXT("MemberName=\"Update_MotionMatching_PostSelection\"")));
	TestTrue(TEXT("callback remains a self-context reference"),
		ImportedReference.Contains(TEXT("bSelfContext=True")));

	const TSharedPtr<FAnimGraphAST> ReExportedAST = FAnimBPExporter::ExportToAST(Destination);
	TestTrue(TEXT("destination re-exports"), ReExportedAST.IsValid());
	if (!ReExportedAST.IsValid()) return false;
	const FString ReExportedDSL = ReExportedAST->ToString();
	TestTrue(TEXT("re-export preserves the Motion Matching callback"),
		ReExportedDSL.Contains(TEXT("on-motion-matching-state-updated-function-ref"))
		&& ReExportedDSL.Contains(TEXT("Update_MotionMatching_PostSelection")));
	TestTrue(TEXT("re-exported callback uses the same structured member-ref"),
		FindMotionMatchingCallbackLine(ReExportedDSL).Contains(
			TEXT("(member-ref :name \"Update_MotionMatching_PostSelection\" :self true)")));
	return true;
}

#endif
