// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimGraphNode_Base.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FString NormalizePinName(const FString& Value)
	{
		FString Result = Value.ToLower();
		Result.ReplaceInline(TEXT("_"), TEXT(""));
		Result.ReplaceInline(TEXT("-"), TEXT(""));
		Result.ReplaceInline(TEXT(" "), TEXT(""));
		return Result;
	}

	UAnimGraphNode_Base* FindControlRigNode(UAnimBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(GraphNode);
				if (Node && Node->GetClass()->GetName() == TEXT("AnimGraphNode_ControlRig")) return Node;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindInputPin(UAnimGraphNode_Base* Node, const FString& Name)
	{
		const FString NormalizedName = NormalizePinName(Name);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input
				&& NormalizePinName(Pin->PinName.ToString()) == NormalizedName)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	FString ExportControlRigReference(UAnimGraphNode_Base* Node)
	{
		for (TFieldIterator<FStructProperty> StructIt(Node->GetClass()); StructIt; ++StructIt)
		{
			FStructProperty* NodeProperty = *StructIt;
			if (!NodeProperty->Struct || !NodeProperty->Struct->IsChildOf(FAnimNode_Base::StaticStruct())) continue;
			if (FProperty* ReferenceProperty = NodeProperty->Struct->FindPropertyByName(TEXT("ControlRigAssetReference")))
			{
				FString Value;
				void* NodeMemory = NodeProperty->ContainerPtrToValuePtr<void>(Node);
				ReferenceProperty->ExportText_Direct(Value,
					ReferenceProperty->ContainerPtrToValuePtr<void>(NodeMemory), nullptr, Node, PPF_None);
				return Value;
			}
		}
		return FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPControlRigInputsRoundTrip,
	"AnimBP2FP.ControlRig.InputsRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPControlRigInputsRoundTrip::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Source = LoadObject<UAnimBlueprint>(
		nullptr, TEXT("/Game/Blueprints/SandboxCharacter_Mover_ABP.SandboxCharacter_Mover_ABP"));
	TestNotNull(TEXT("real Mover AnimBlueprint loads"), Source);
	if (!Source) return false;

	UAnimGraphNode_Base* SourceNode = FindControlRigNode(Source);
	TestNotNull(TEXT("source Control Rig node exists"), SourceNode);
	if (!SourceNode) return false;
	const FString SourceReference = ExportControlRigReference(SourceNode);
	TestTrue(TEXT("source Control Rig asset reference is populated"),
		SourceReference.Contains(TEXT("CR_Biped_FootPlacement_C")));

	const TSharedPtr<FAnimGraphAST> SourceAST = FAnimBPExporter::ExportToAST(Source);
	TestTrue(TEXT("real Mover AnimBlueprint exports"), SourceAST.IsValid());
	if (!SourceAST.IsValid()) return false;
	TestTrue(TEXT("Control Rig DSL preserves the exposed input manifest"),
		SourceAST->ToString().Contains(TEXT(":exposed-input-pins (pin-names")));

	UClass* ParentClass = Source->ParentClass ? Source->ParentClass.Get() : UAnimInstance::StaticClass();
	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		ParentClass, GetTransientPackage(), TEXT("ABP_ControlRigInputsRoundTrip"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPControlRigTest"))));
	TestNotNull(TEXT("transient destination AnimBlueprint is created"), Destination);
	if (!Destination) return false;
	Destination->TargetSkeleton = Source->TargetSkeleton;

	const FAnimBPImporter::FUpdateResult ImportResult =
		FAnimBPImporter::UpdateBlueprintDetailed(Destination, SourceAST->ToString());
	TestTrue(TEXT("real Mover DSL import succeeds"), ImportResult.bSuccess);

	UAnimGraphNode_Base* DestinationNode = FindControlRigNode(Destination);
	TestNotNull(TEXT("destination Control Rig node exists"), DestinationNode);
	if (!DestinationNode) return false;
	TestEqual(TEXT("Control Rig asset reference round-trips"),
		ExportControlRigReference(DestinationNode), SourceReference);

	const TArray<FString> LinkedInputs = {
		TEXT("GroundNormal"), TEXT("DebugDraw"), TEXT("EnableFootPinning"),
		TEXT("EnableSlopeWarping"), TEXT("HasTeleported"),
		TEXT("WorldZDamperEnabled"), TEXT("ForceReset") };
	for (const FString& InputName : LinkedInputs)
	{
		UEdGraphPin* SourcePin = FindInputPin(SourceNode, InputName);
		UEdGraphPin* DestinationPin = FindInputPin(DestinationNode, InputName);
		TestNotNull(TEXT("source Control Rig input exists: ") + InputName, SourcePin);
		TestNotNull(TEXT("destination Control Rig input exists: ") + InputName, DestinationPin);
		if (SourcePin) TestTrue(TEXT("source Control Rig input is linked: ") + InputName, SourcePin->LinkedTo.Num() > 0);
		if (DestinationPin) TestTrue(TEXT("destination Control Rig input is linked: ") + InputName, DestinationPin->LinkedTo.Num() > 0);
	}
	UEdGraphPin* SourceRaycast = FindInputPin(SourceNode, TEXT("DoRaycast"));
	UEdGraphPin* DestinationRaycast = FindInputPin(DestinationNode, TEXT("DoRaycast"));
	TestNotNull(TEXT("source DoRaycast input exists"), SourceRaycast);
	TestNotNull(TEXT("destination DoRaycast input exists"), DestinationRaycast);
	if (SourceRaycast && DestinationRaycast)
	{
		TestEqual(TEXT("DoRaycast default round-trips"), DestinationRaycast->DefaultValue, SourceRaycast->DefaultValue);
	}
	return true;
}

#endif
