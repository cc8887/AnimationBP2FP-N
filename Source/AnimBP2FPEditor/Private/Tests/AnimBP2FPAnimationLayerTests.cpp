// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPVersionCompat.h"
#if ANIMBP2FP_HAS_ANIM_AUTHORING
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Animation/AnimBlueprint.h"
#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "AnimLangParser.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_LocalRefPose.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPExportsNamedAnimationLayerGraphs,
	"AnimBP2FP.AnimationLayers.ExportsAllImplementedInterfaceGraphs",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPExportsNamedAnimationLayerGraphs::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Blueprint = NewObject<UAnimBlueprint>(GetTransientPackage(), TEXT("ABP_AnimationLayerExportFixture"));
	UEdGraph* LayerGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("UpperBodyLayer"), UEdGraph::StaticClass(), UAnimationGraphSchema::StaticClass());

	UAnimGraphNode_Root* Root = NewObject<UAnimGraphNode_Root>(LayerGraph);
	Root->CreateNewGuid();
	Root->PostPlacedNewNode();
	Root->AllocateDefaultPins();
	LayerGraph->AddNode(Root, false, false);
	UAnimGraphNode_LocalRefPose* Identity = NewObject<UAnimGraphNode_LocalRefPose>(LayerGraph);
	Identity->CreateNewGuid();
	Identity->PostPlacedNewNode();
	Identity->AllocateDefaultPins();
	LayerGraph->AddNode(Identity, false, false);

	UEdGraphPin* PoseOutput = nullptr;
	UEdGraphPin* PoseInput = nullptr;
	for (UEdGraphPin* Pin : Identity->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			PoseOutput = Pin;
			break;
		}
	}
	for (UEdGraphPin* Pin : Root->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			PoseInput = Pin;
			break;
		}
	}
	if (PoseOutput && PoseInput)
	{
		PoseOutput->MakeLinkTo(PoseInput);
	}
	TestTrue(TEXT("layer pose connects to its root"), PoseOutput && PoseInput && PoseInput->LinkedTo.Contains(PoseOutput));

	FBPInterfaceDescription& InterfaceDesc = Blueprint->ImplementedInterfaces.AddDefaulted_GetRef();
	InterfaceDesc.Interface = UInterface::StaticClass();
	InterfaceDesc.Graphs.Add(LayerGraph);

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("animation layer AST is exported"), AST.IsValid() && AST->AnimationLayers.Num() == 1);
	if (!AST.IsValid() || AST->AnimationLayers.Num() != 1)
	{
		return false;
	}
	TestEqual(TEXT("layer graph name is preserved"), AST->AnimationLayers[0].GraphName, FString(TEXT("UpperBodyLayer")));
	TestEqual(TEXT("layer interface path is preserved"), AST->AnimationLayers[0].InterfaceClassPath, UInterface::StaticClass()->GetPathName());
	TestTrue(TEXT("layer root pose is semantic"), AST->AnimationLayers[0].RootNode.IsValid());

	const FString DSL = AST->ToString();
	TestTrue(TEXT("main DSL contains animation-layers block"), DSL.Contains(TEXT("(animation-layers")));
	TestTrue(TEXT("main DSL contains the layer name"), DSL.Contains(TEXT("UpperBodyLayer")));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("animation layer DSL parses"), Parsed.IsValid() && Errors.Num() == 0);
	TestTrue(TEXT("parsed AST preserves the layer"), Parsed.IsValid() && Parsed->AnimationLayers.Num() == 1);
	TestTrue(TEXT("parsed layer preserves its root"), Parsed.IsValid() && Parsed->AnimationLayers.Num() == 1
		&& Parsed->AnimationLayers[0].RootNode.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPExportsSelfAnimationLayerGraphs,
	"AnimBP2FP.AnimationLayers.ExportsSelfAnimationLayerGraph",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPExportsSelfAnimationLayerGraphs::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Blueprint = NewObject<UAnimBlueprint>(GetTransientPackage(), TEXT("ABP_SelfAnimationLayerExportFixture"));
	UAnimationGraph* LayerGraph = Cast<UAnimationGraph>(FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("Procedural"), UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass()));
	TestNotNull(TEXT("self layer graph is created"), LayerGraph);
	if (!LayerGraph) return false;
	FBlueprintEditorUtils::AddDomainSpecificGraph(Blueprint, LayerGraph);

	UAnimGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* Node : LayerGraph->Nodes)
	{
		Root = Cast<UAnimGraphNode_Root>(Node);
		if (Root) break;
	}
	UAnimGraphNode_LocalRefPose* Identity = NewObject<UAnimGraphNode_LocalRefPose>(LayerGraph);
	Identity->CreateNewGuid();
	Identity->PostPlacedNewNode();
	Identity->AllocateDefaultPins();
	LayerGraph->AddNode(Identity, false, false);
	UEdGraphPin* PoseOutput = Identity->FindPin(TEXT("Pose"), EGPD_Output);
	UEdGraphPin* PoseInput = Root ? Root->FindPin(TEXT("Result"), EGPD_Input) : nullptr;
	if (!PoseOutput)
	{
		for (UEdGraphPin* Pin : Identity->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				PoseOutput = Pin;
				break;
			}
		}
	}
	if (!PoseInput && Root)
	{
		for (UEdGraphPin* Pin : Root->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
			{
				PoseInput = Pin;
				break;
			}
		}
	}
	if (PoseOutput && PoseInput) PoseOutput->MakeLinkTo(PoseInput);

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("self animation layer is exported"), AST.IsValid() && AST->AnimationLayers.Num() == 1);
	if (!AST.IsValid() || AST->AnimationLayers.Num() != 1) return false;
	TestEqual(TEXT("self layer name is preserved"), AST->AnimationLayers[0].GraphName, FString(TEXT("Procedural")));
	TestTrue(TEXT("self layer has no interface owner"), AST->AnimationLayers[0].InterfaceClassPath.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPImportsNamedAnimationLayerGraphs,
	"AnimBP2FP.AnimationLayers.ImporterRestoresNamedPoseGraph",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPImportsNamedAnimationLayerGraphs::RunTest(const FString& Parameters)
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_AnimationLayerImportFixture");
	FAnimationLayerDef& Layer = AST->AnimationLayers.AddDefaulted_GetRef();
	Layer.GraphName = TEXT("UpperBodyLayer");
	Layer.SchemaClassPath = UAnimationGraphSchema::StaticClass()->GetPathName();
	Layer.GraphGuid = FGuid::NewGuid().ToString(ANIMBP2FP_GUID_HYPHENS);
	Layer.RootNode = MakeShared<FAnimNodeAST>();
	Layer.RootNode->NodeType = TEXT("identity-pose");
	AST->RootNode = MakeShared<FAnimNodeAST>();
	AST->RootNode->NodeType = TEXT("linked-anim-layer");
	AST->RootNode->NodeClassPath = UAnimGraphNode_LinkedAnimLayer::StaticClass()->GetPathName();
	AST->RootNode->Properties.Add(TEXT("layer"), TEXT("\"UpperBodyLayer\""));

	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_AnimationLayerImportDestination"),
		BPTYPE_Normal, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("AnimBP2FPAnimationLayerTest"))));
	TestNotNull(TEXT("destination AnimBlueprint exists"), Destination);
	if (!Destination) return false;

	const FAnimBPImporter::FUpdateResult Result = FAnimBPImporter::UpdateBlueprintDetailed(Destination, AST->ToString());
	TestTrue(TEXT("animation layer import succeeds"), Result.bSuccess);

	UEdGraph* RestoredLayer = nullptr;
	for (UEdGraph* Graph : Destination->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == TEXT("UpperBodyLayer"))
		{
			RestoredLayer = Graph;
			break;
		}
	}
	TestNotNull(TEXT("named animation layer graph is recreated"), RestoredLayer);
	TestTrue(TEXT("self animation layer uses UAnimationGraph"), RestoredLayer && RestoredLayer->IsA<UAnimationGraph>());
	TestTrue(TEXT("restored layer uses animation graph schema"), RestoredLayer && RestoredLayer->GetSchema()
		&& RestoredLayer->GetSchema()->IsA<UAnimationGraphSchema>());
	TestEqual(TEXT("restored layer graph GUID is exact"), RestoredLayer ? RestoredLayer->GraphGuid.ToString(ANIMBP2FP_GUID_HYPHENS) : FString(), Layer.GraphGuid);
	TestTrue(TEXT("restored layer contains a root node"), RestoredLayer && RestoredLayer->Nodes.ContainsByPredicate(
		[](const UEdGraphNode* Node) { return Node && Node->IsA<UAnimGraphNode_Root>(); }));
	return true;
}

#endif

#endif // ANIMBP2FP_HAS_ANIM_AUTHORING
