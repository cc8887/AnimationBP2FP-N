// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimLangAST.h"
#include "AnimLangParser.h"
#include "AnimBPImporter.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_LocalRefPose.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/KismetEditorUtilities.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPNodeIdRoundTrips,
	"AnimBP2FP.AnimationMetadata.NodeIdRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPNodeIdRoundTrips::RunTest(const FString& Parameters)
{
	const FGuid ExpectedGuid(0xA1B2C3D4, 0x10203040, 0x50607080, 0x90ABCDEF);
	const FString ExpectedNodeId = ExpectedGuid.ToString();
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_NodeIdFixture");
	AST->RootNode = MakeShared<FAnimNodeAST>();
	AST->RootNode->NodeType = TEXT("local-ref-pose");
	AST->RootNode->NodeClassPath = UAnimGraphNode_LocalRefPose::StaticClass()->GetPathName();
	AST->RootNode->Coverage = EAnimNodeCoverage::Reflected;
	AST->RootNode->NodeId = ExpectedNodeId;

	const FString DSL = AST->ToString();
	TestTrue(TEXT("full node id is serialized"), DSL.Contains(FString::Printf(TEXT(":node-id \"%s\""), *ExpectedNodeId)));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("node id DSL parses"), Parsed.IsValid() && Errors.IsEmpty());
	TestTrue(TEXT("parsed root exists"), Parsed.IsValid() && Parsed->RootNode.IsValid());
	if (!Parsed.IsValid() || !Parsed->RootNode.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("node id survives roundtrip"), Parsed->RootNode->NodeId, AST->RootNode->NodeId);

	UAnimBlueprint* Destination = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_NodeIdAssetRoundTrip"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(), FName(TEXT("AnimBP2FPAnimationMetadataTest"))));
	TestNotNull(TEXT("asset fixture exists"), Destination);
	if (!Destination)
	{
		return false;
	}

	const FAnimBPImporter::FUpdateResult Result = FAnimBPImporter::UpdateBlueprintDetailed(Destination, DSL);
	TestTrue(TEXT("asset import succeeds"), Result.bSuccess);
	bool bFoundStableGuid = false;
	for (UEdGraph* Graph : Destination->FunctionGraphs)
	{
		if (!Graph || !Graph->GetSchema() || !Graph->GetSchema()->IsA<UAnimationGraphSchema>())
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<UAnimGraphNode_LocalRefPose>())
			{
				bFoundStableGuid |= Node->NodeGuid == ExpectedGuid;
			}
		}
	}
	TestTrue(TEXT("NodeId restores the real editor node guid"), bFoundStableGuid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPDependencyManifestRoundTrips,
	"AnimBP2FP.AnimationMetadata.DependencyManifestRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPDependencyManifestRoundTrips::RunTest(const FString& Parameters)
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_DependencyFixture");
	AST->Metadata.RootMotionMode = TEXT("RootMotionFromMontagesOnly");

	FAnimDependency Sequence;
	Sequence.ObjectPath = TEXT("/Game/Animations/AS_Run.AS_Run");
	Sequence.ClassPath = TEXT("/Script/Engine.AnimSequence");
	Sequence.Role = TEXT("anim-sequence");
	Sequence.Mode = TEXT("external");
	Sequence.AssetMetadata.bHasSnapshot = true;
	Sequence.AssetMetadata.bHasRootMotion = true;
	Sequence.AssetMetadata.bEnableRootMotion = true;
	Sequence.AssetMetadata.RootMotionRootLock = TEXT("AnimFirstFrame");
	Sequence.AssetMetadata.SyncMarkers.Add({TEXT("LeftFoot"), 0.25f});
	FAnimNotifySnapshot Notify;
	Notify.ClassPath = TEXT("/Script/Engine.AnimNotify_PlaySound");
	Notify.Name = TEXT("Footstep");
	Notify.Time = 0.25f;
	Sequence.AssetMetadata.Notifies.Add(Notify);
	AST->Dependencies.Add(Sequence);

	FAnimDependency Montage;
	Montage.ObjectPath = TEXT("/Game/Animations/AM_Attack.AM_Attack");
	Montage.ClassPath = TEXT("/Script/Engine.AnimMontage");
	Montage.Role = TEXT("montage");
	Montage.Mode = TEXT("external");
	Montage.AssetMetadata.bHasSnapshot = true;
	Montage.AssetMetadata.MontageSections.Add({TEXT("Start"), 0.0f, TEXT("End")});
	Montage.AssetMetadata.SlotTrackNames.Add(TEXT("DefaultSlot"));
	AST->Dependencies.Add(Montage);

	const FString DSL = AST->ToString();
	TestTrue(TEXT("AnimBlueprint metadata is serialized"), DSL.Contains(TEXT(":root-motion-mode \"RootMotionFromMontagesOnly\"")));
	TestTrue(TEXT("dependencies use external mode"), DSL.Contains(TEXT(":mode external")));
	TestTrue(TEXT("dependencies are stably sorted by object path"),
		DSL.Find(Montage.ObjectPath) < DSL.Find(Sequence.ObjectPath));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("dependency DSL parses"), Parsed.IsValid() && Errors.IsEmpty());
	if (!Parsed.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("root motion mode survives"), Parsed->Metadata.RootMotionMode, AST->Metadata.RootMotionMode);
	TestEqual(TEXT("dependency count survives"), Parsed->Dependencies.Num(), 2);
	if (Parsed->Dependencies.Num() != 2)
	{
		return false;
	}
	TestEqual(TEXT("stable first dependency"), Parsed->Dependencies[0].ObjectPath, Montage.ObjectPath);
	TestEqual(TEXT("snapshot notify survives"), Parsed->Dependencies[1].AssetMetadata.Notifies[0].Name, FString(TEXT("Footstep")));
	TestEqual(TEXT("sync marker survives"), Parsed->Dependencies[1].AssetMetadata.SyncMarkers[0].Name, FString(TEXT("LeftFoot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPMissingDependencyFailsBeforeImport,
	"AnimBP2FP.AnimationMetadata.MissingDependencyFailsBeforeImport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPMissingDependencyFailsBeforeImport::RunTest(const FString& Parameters)
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_MissingDependencyFixture");
	FAnimDependency& Missing = AST->Dependencies.AddDefaulted_GetRef();
	Missing.ObjectPath = TEXT("/Game/DoesNotExist/AS_Missing.AS_Missing");
	Missing.ClassPath = TEXT("/Script/Engine.AnimSequence");
	Missing.Role = TEXT("anim-sequence");
	Missing.Mode = TEXT("external");

	FString Error;
	AddExpectedError(TEXT("Missing dependency"), EAutomationExpectedErrorFlags::Contains, 1);
	UAnimBlueprint* Imported = FAnimBPImporter::ImportFromAST(AST, TEXT("/Game/__AnimBP2FPTests"), &Error);
	TestNull(TEXT("missing dependency rejects import"), Imported);
	TestTrue(TEXT("error identifies dependency validation"), Error.Contains(TEXT("dependency"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("error identifies missing object"), Error.Contains(Missing.ObjectPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPRootMotionMetadataUpdatesExistingBlueprint,
	"AnimBP2FP.AnimationMetadata.RootMotionMetadataUpdatesExistingBlueprint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPRootMotionMetadataUpdatesExistingBlueprint::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), GetTransientPackage(), TEXT("ABP_RootMotionMetadataUpdate"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(), FName(TEXT("AnimBP2FPAnimationMetadataTest"))));
	TestNotNull(TEXT("fixture AnimBlueprint exists"), Blueprint);
	if (!Blueprint) return false;

	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = Blueprint->GetName();
	AST->Metadata.RootMotionMode = TEXT("RootMotionFromEverything");
	const FAnimBPImporter::FUpdateResult Result = FAnimBPImporter::UpdateBlueprintDetailed(Blueprint, AST->ToString());
	TestTrue(TEXT("metadata-only update succeeds"), Result.bSuccess);
	TestTrue(TEXT("metadata-only update is structural"), Result.NumStructuralChanges > 0);

	UObject* Defaults = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject(false) : nullptr;
	FProperty* Property = Defaults ? FindFProperty<FProperty>(Defaults->GetClass(), TEXT("RootMotionMode")) : nullptr;
	FString Actual;
	if (Property)
	{
		Property->ExportText_Direct(Actual, Property->ContainerPtrToValuePtr<void>(Defaults), nullptr, Defaults, PPF_None);
	}
	TestEqual(TEXT("RootMotionMode is restored on class defaults"), Actual, AST->Metadata.RootMotionMode);
	return true;
}

#endif
