// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_LocalRefPose.h"
#include "AnimGraphNode_Root.h"
#include "AnimLangParser.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AnimBP2FPFallbackSemanticsTests
{
	static UAnimBlueprint* NewAnimBlueprint(const FName Name)
	{
		return Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UAnimInstance::StaticClass(),
			GetTransientPackage(),
			Name,
			BPTYPE_Normal,
			UAnimBlueprint::StaticClass(),
			UAnimBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("AnimBP2FPFallbackSemanticsTest"))));
	}

	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FName Name, const EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName == Name && Pin->Direction == Direction)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UEdGraph* FindAnimGraph(UAnimBlueprint* Blueprint)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetSchema() && Graph->GetSchema()->IsA<UAnimationGraphSchema>())
			{
				return Graph;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FAnimNodeAST> FindNodeByClassPath(const TSharedPtr<FAnimNodeAST>& Node, const FString& ClassPath)
	{
		if (!Node.IsValid() || Node->NodeClassPath == ClassPath)
		{
			return Node;
		}
		for (const FNamedChild& Child : Node->Children)
		{
			if (TSharedPtr<FAnimNodeAST> Match = FindNodeByClassPath(Child.Node, ClassPath))
			{
				return Match;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPGenericMetadataRoundTrips,
	"AnimBP2FP.FallbackSemantics.GenericMetadataRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPGenericMetadataRoundTrips::RunTest(const FString& Parameters)
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_GenericMetadata");
	AST->RootNode = MakeShared<FAnimNodeAST>();
	AST->RootNode->NodeType = TEXT("modify-bone");
	AST->RootNode->NodeClassPath = UAnimGraphNode_ModifyBone::StaticClass()->GetPathName();
	AST->RootNode->Coverage = EAnimNodeCoverage::Reflected;

	const FString DSL = AST->ToString();
	TestTrue(TEXT("node class path is serialized"), DSL.Contains(TEXT(":node-class \"/Script/AnimGraph.AnimGraphNode_ModifyBone\"")));
	TestTrue(TEXT("coverage is serialized"), DSL.Contains(TEXT(":coverage reflected")));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("generic metadata DSL parses"), Parsed.IsValid() && Errors.IsEmpty());
	if (!Parsed.IsValid() || !Parsed->RootNode.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("class path survives parsing"), Parsed->RootNode->NodeClassPath, AST->RootNode->NodeClassPath);
	TestEqual(TEXT("coverage survives parsing"), static_cast<uint8>(Parsed->RootNode->Coverage), static_cast<uint8>(EAnimNodeCoverage::Reflected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPGenericStructDefaultIsExported,
	"AnimBP2FP.FallbackSemantics.GenericStructDefaultIsExported",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPGenericStructDefaultIsExported::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPFallbackSemanticsTests;

	UAnimBlueprint* Blueprint = NewAnimBlueprint(TEXT("ABP_GenericStructDefault"));
	TestNotNull(TEXT("fixture blueprint is created"), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	UEdGraph* Graph = FindAnimGraph(Blueprint);
	TestNotNull(TEXT("factory creates an AnimGraph"), Graph);
	if (!Graph)
	{
		return false;
	}
	UAnimGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		Root = Cast<UAnimGraphNode_Root>(Node);
		if (Root)
		{
			break;
		}
	}
	TestNotNull(TEXT("factory creates the root node"), Root);
	if (!Root)
	{
		return false;
	}

	UAnimGraphNode_ModifyBone* ModifyBone = NewObject<UAnimGraphNode_ModifyBone>(Graph);
	ModifyBone->CreateNewGuid();
	ModifyBone->AllocateDefaultPins();
	Graph->AddNode(ModifyBone, false, false);

	UEdGraphPin* Translation = FindPin(ModifyBone, TEXT("Translation"), EGPD_Input);
	TestNotNull(TEXT("ModifyBone exposes Translation struct pin"), Translation);
	if (!Translation)
	{
		return false;
	}
	Translation->DefaultValue = TEXT("(X=1.000000,Y=2.000000,Z=3.000000)");

	UEdGraphPin* PoseOutput = nullptr;
	for (UEdGraphPin* Pin : ModifyBone->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			PoseOutput = Pin;
			break;
		}
	}
	UEdGraphPin* RootInput = nullptr;
	for (UEdGraphPin* Pin : Root->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			RootInput = Pin;
			break;
		}
	}
	TestTrue(TEXT("fixture pose connection succeeds"), PoseOutput && RootInput && Graph->GetSchema()->TryCreateConnection(PoseOutput, RootInput));

	const TSharedPtr<FAnimGraphAST> Exported = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("fixture exports a root node"), Exported.IsValid() && Exported->RootNode.IsValid());
	if (!Exported.IsValid() || !Exported->RootNode.IsValid())
	{
		return false;
	}
	const TSharedPtr<FAnimNodeAST> ModifyBoneAST = FindNodeByClassPath(Exported->RootNode, ModifyBone->GetClass()->GetPathName());
	TestTrue(TEXT("generic class path is preserved"), ModifyBoneAST.IsValid());
	if (!ModifyBoneAST.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("generic export is reflected coverage"), static_cast<uint8>(ModifyBoneAST->Coverage), static_cast<uint8>(EAnimNodeCoverage::Reflected));
	const FString* TranslationValue = ModifyBoneAST->Properties.Find(TEXT("translation"));
	TestNotNull(TEXT("unconnected struct default is collected"), TranslationValue);
	TestTrue(TEXT("struct default value is preserved"), TranslationValue && TranslationValue->Contains(TEXT("X=1.000000")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPClassPathTakesPriorityOnImport,
	"AnimBP2FP.FallbackSemantics.ClassPathTakesPriorityOnImport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPClassPathTakesPriorityOnImport::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPFallbackSemanticsTests;
	UAnimBlueprint* Destination = NewAnimBlueprint(TEXT("ABP_ClassPathImport"));
	TestNotNull(TEXT("destination blueprint is created"), Destination);
	if (!Destination)
	{
		return false;
	}

	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = Destination->GetName();
	AST->RootNode = MakeShared<FAnimNodeAST>();
	AST->RootNode->NodeType = TEXT("intentionally-unresolvable-alias");
	AST->RootNode->NodeClassPath = UAnimGraphNode_LocalRefPose::StaticClass()->GetPathName();
	AST->RootNode->Coverage = EAnimNodeCoverage::Reflected;

	const FAnimBPImporter::FUpdateResult Result = FAnimBPImporter::UpdateBlueprintDetailed(Destination, AST->ToString());
	TestTrue(TEXT("valid node class path imports even when alias is unknown"), Result.bSuccess);

	bool bFoundRefPose = false;
	for (UEdGraph* Graph : Destination->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			bFoundRefPose |= Node && Node->IsA<UAnimGraphNode_LocalRefPose>();
		}
	}
	TestTrue(TEXT("class-path class is instantiated"), bFoundRefPose);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPUnknownNodeDoesNotBecomeCachedPose,
	"AnimBP2FP.FallbackSemantics.UnknownNodeDoesNotBecomeCachedPose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPUnknownNodeDoesNotBecomeCachedPose::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPFallbackSemanticsTests;
	UAnimBlueprint* Destination = NewAnimBlueprint(TEXT("ABP_UnknownNodeImport"));
	TestNotNull(TEXT("destination blueprint is created"), Destination);
	if (!Destination)
	{
		return false;
	}

	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = Destination->GetName();
	AST->RootNode = MakeShared<FAnimNodeAST>();
	AST->RootNode->NodeType = TEXT("Post-Layering");
	AST->RootNode->Coverage = EAnimNodeCoverage::Unsupported;

	const FAnimBPImporter::FUpdateResult Result = FAnimBPImporter::UpdateBlueprintDetailed(Destination, AST->ToString());
	TestFalse(TEXT("unknown parenthesized or bare-looking node is a hard import failure"), Result.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPBlendStackEmptyBoundGraphIsUnsupported,
	"AnimBP2FP.FallbackSemantics.BlendStackEmptyBoundGraphIsUnsupported",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPBlendStackEmptyBoundGraphIsUnsupported::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPFallbackSemanticsTests;
	FModuleManager::Get().LoadModule(TEXT("BlendStackEditor"));
	UClass* BlendStackClass = LoadClass<UAnimGraphNode_Base>(nullptr, TEXT("/Script/BlendStackEditor.AnimGraphNode_BlendStack"));
	TestNotNull(TEXT("UE 5.9 BlendStack editor class loads"), BlendStackClass);
	if (!BlendStackClass)
	{
		return false;
	}

	UAnimBlueprint* Blueprint = NewAnimBlueprint(TEXT("ABP_BlendStackFallback"));
	UEdGraph* Graph = Blueprint ? FindAnimGraph(Blueprint) : nullptr;
	TestNotNull(TEXT("fixture AnimGraph exists"), Graph);
	if (!Graph)
	{
		return false;
	}

	UAnimGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		Root = Cast<UAnimGraphNode_Root>(Node);
		if (Root) break;
	}
	TestNotNull(TEXT("fixture root exists"), Root);
	if (!Root)
	{
		return false;
	}

	UAnimGraphNode_Base* BlendStack = NewObject<UAnimGraphNode_Base>(Graph, BlendStackClass);
	BlendStack->CreateNewGuid();
	BlendStack->PostPlacedNewNode();
	BlendStack->AllocateDefaultPins();
	Graph->AddNode(BlendStack, false, false);
	TestTrue(TEXT("BlendStack creates its bound subgraph"), !BlendStack->GetSubGraphs().IsEmpty());

	UEdGraphPin* Output = nullptr;
	for (UEdGraphPin* Pin : BlendStack->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			Output = Pin;
			break;
		}
	}
	UEdGraphPin* RootInput = nullptr;
	for (UEdGraphPin* Pin : Root->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			RootInput = Pin;
			break;
		}
	}
	TestTrue(TEXT("BlendStack connects to graph root"), Output && RootInput && Graph->GetSchema()->TryCreateConnection(Output, RootInput));

	const TSharedPtr<FAnimGraphAST> Exported = FAnimBPExporter::ExportToAST(Blueprint);
	const TSharedPtr<FAnimNodeAST> BlendStackAST = Exported.IsValid()
		? FindNodeByClassPath(Exported->RootNode, BlendStackClass->GetPathName())
		: nullptr;
	TestTrue(TEXT("BlendStack class path is exported"), BlendStackAST.IsValid());
	if (!BlendStackAST.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("BlendStack bound pose graph is semantically supported"), BlendStackAST->Coverage != EAnimNodeCoverage::Unsupported);
	const FNamedChild* SampleGraph = BlendStackAST->Children.FindByPredicate(
		[](const FNamedChild& Child) { return Child.PinName == TEXT("sample-graph"); });
	TestTrue(TEXT("BlendStack exports its sample pose graph"), SampleGraph && SampleGraph->Node.IsValid());
	TestTrue(TEXT("default sample graph preserves BlendStackInput"), SampleGraph && SampleGraph->Node.IsValid()
		&& SampleGraph->Node->NodeClassPath.Contains(TEXT("AnimGraphNode_BlendStackInput")));
	TestTrue(TEXT("bound graph class is recorded"), BlendStackAST->Properties.Contains(TEXT("bound-graph-class")));
	TestTrue(TEXT("bound graph schema is recorded"), BlendStackAST->Properties.Contains(TEXT("bound-graph-schema")));
	TestTrue(TEXT("bound graph GUID is recorded"), BlendStackAST->Properties.Contains(TEXT("bound-graph-guid")));

	UAnimBlueprint* ImportDestination = NewAnimBlueprint(TEXT("ABP_BlendStackLossyImport"));
	const FAnimBPImporter::FUpdateResult ImportResult = FAnimBPImporter::UpdateBlueprintDetailed(ImportDestination, Exported->ToString());
	TestTrue(TEXT("semantic BlendStack export imports"), ImportResult.bSuccess);
	const TSharedPtr<FAnimGraphAST> ReExported = ImportResult.bSuccess ? FAnimBPExporter::ExportToAST(ImportDestination) : nullptr;
	const TSharedPtr<FAnimNodeAST> ReExportedBlendStack = ReExported.IsValid()
		? FindNodeByClassPath(ReExported->RootNode, BlendStackClass->GetPathName()) : nullptr;
	TestTrue(TEXT("imported BlendStack re-exports"), ReExportedBlendStack.IsValid());
	TestTrue(TEXT("re-export preserves the sample graph"), ReExportedBlendStack.IsValid()
		&& ReExportedBlendStack->Children.ContainsByPredicate(
			[](const FNamedChild& Child) { return Child.PinName == TEXT("sample-graph") && Child.Node.IsValid(); }));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
