// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "AnimLangAST.h"
#include "AnimLangParser.h"
#include "AnimBPExporter.h"
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
	FAnimBP2FPTypedAssetSnapshotRoundTrips,
	"AnimBP2FP.AnimationMetadata.TypedAssetSnapshotRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPTypedAssetSnapshotRoundTrips::RunTest(const FString& Parameters)
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_TypedAssetSnapshotFixture");

	FAnimDependency& Chooser = AST->Dependencies.AddDefaulted_GetRef();
	Chooser.ObjectPath = TEXT("/Game/Choosers/CHT_Locomotion.CHT_Locomotion");
	Chooser.ClassPath = TEXT("/Script/Chooser.ChooserTable");
	Chooser.Role = TEXT("chooser");
	Chooser.TypedSnapshot.bHasSnapshot = true;
	Chooser.TypedSnapshot.Kind = TEXT("chooser");
	Chooser.TypedSnapshot.StableHash = TEXT("0123456789abcdef0123456789abcdef01234567");
	Chooser.TypedSnapshot.Fields.Add({TEXT("ColumnsStructs[0]"), TEXT("FInstancedStruct"), TEXT("(Value=Locomotion)")});
	Chooser.TypedSnapshot.ObjectReferences.Add(TEXT("/Game/PoseSearch/PSD_Locomotion.PSD_Locomotion"));

	const FString DSL = AST->ToString();
	TestTrue(TEXT("typed snapshot is serialized"), DSL.Contains(TEXT(":typed-snapshot (asset-structure")));
	TestTrue(TEXT("typed field path is serialized"), DSL.Contains(TEXT(":path \"ColumnsStructs[0]\"")));
	TestTrue(TEXT("candidate object reference is serialized"), DSL.Contains(Chooser.TypedSnapshot.ObjectReferences[0]));

	TArray<FAnimLangParseError> Errors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(DSL, Errors);
	TestTrue(TEXT("typed snapshot DSL parses"), Parsed.IsValid() && Errors.IsEmpty());
	if (!Parsed.IsValid() || Parsed->Dependencies.Num() != 1)
	{
		return false;
	}

	const FExternalAssetTypedSnapshot& Snapshot = Parsed->Dependencies[0].TypedSnapshot;
	TestTrue(TEXT("typed snapshot survives"), Snapshot.bHasSnapshot);
	TestEqual(TEXT("snapshot kind survives"), Snapshot.Kind, FString(TEXT("chooser")));
	TestEqual(TEXT("stable hash survives"), Snapshot.StableHash, Chooser.TypedSnapshot.StableHash);
	TestEqual(TEXT("field count survives"), Snapshot.Fields.Num(), 1);
	TestEqual(TEXT("field type survives"), Snapshot.Fields[0].Type, FString(TEXT("FInstancedStruct")));
	TestEqual(TEXT("object reference survives"), Snapshot.ObjectReferences[0], Chooser.TypedSnapshot.ObjectReferences[0]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimBP2FPRealMotionMatchingAssetsHaveTypedSnapshots,
	"AnimBP2FP.AnimationMetadata.RealMotionMatchingAssetsHaveTypedSnapshots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAnimBP2FPRealMotionMatchingAssetsHaveTypedSnapshots::RunTest(const FString& Parameters)
{
	UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, TEXT("/Game/Blueprints/SandboxCharacter_CMC_ABP.SandboxCharacter_CMC_ABP"));
	TestNotNull(TEXT("real CMC AnimBlueprint loads"), Blueprint);
	if (!Blueprint) return false;

	const TSharedPtr<FAnimGraphAST> AST = FAnimBPExporter::ExportToAST(Blueprint);
	TestTrue(TEXT("real CMC AnimBlueprint exports"), AST.IsValid());
	if (!AST.IsValid()) return false;
	TestTrue(TEXT("database animation candidates are not promoted to top-level dependencies"), AST->Dependencies.Num() < 100);

	auto FindDependency = [&AST](const FString& Role) -> const FAnimDependency*
	{
		return AST->Dependencies.FindByPredicate([&Role](const FAnimDependency& Dependency)
		{
			return Dependency.Role == Role;
		});
	};
	auto HasFieldContaining = [](const FExternalAssetTypedSnapshot& Snapshot, const FString& Token)
	{
		return Snapshot.Fields.ContainsByPredicate([&Token](const FExternalAssetSnapshotField& Field)
		{
			return Field.Path.Contains(Token);
		});
	};
	const FAnimDependency* Database = FindDependency(TEXT("pose-search-database"));
	TestNotNull(TEXT("Chooser candidate database is recursively collected"), Database);
	const FAnimDependency* Chooser = Database ? AST->Dependencies.FindByPredicate([Database](const FAnimDependency& Dependency)
	{
		return Dependency.Role == TEXT("chooser") && Dependency.TypedSnapshot.ObjectReferences.Contains(Database->ObjectPath);
	}) : nullptr;
	TestNotNull(TEXT("Chooser dependency is collected"), Chooser);
	if (Chooser)
	{
		TestTrue(TEXT("Chooser has typed snapshot"), Chooser->TypedSnapshot.bHasSnapshot);
		TestEqual(TEXT("Chooser snapshot kind"), Chooser->TypedSnapshot.Kind, FString(TEXT("chooser")));
		TestEqual(TEXT("Chooser hash is SHA-1 length"), Chooser->TypedSnapshot.StableHash.Len(), 40);
		TestTrue(TEXT("Chooser captures columns"), HasFieldContaining(Chooser->TypedSnapshot, TEXT("ColumnsStructs")));
		TestTrue(TEXT("Chooser captures result rows"), HasFieldContaining(Chooser->TypedSnapshot, TEXT("ResultsStructs")));
	}

	if (Chooser && Database)
	{
		TestTrue(TEXT("Chooser captures the recursively collected database candidate"),
			Chooser->TypedSnapshot.ObjectReferences.Contains(Database->ObjectPath));
	}
	if (Database)
	{
		TestTrue(TEXT("database has typed snapshot"), Database->TypedSnapshot.bHasSnapshot);
		TestEqual(TEXT("database snapshot kind"), Database->TypedSnapshot.Kind, FString(TEXT("pose-search-database")));
		TestEqual(TEXT("database hash is SHA-1 length"), Database->TypedSnapshot.StableHash.Len(), 40);
		TestTrue(TEXT("database captures animation entries"), HasFieldContaining(Database->TypedSnapshot, TEXT("DatabaseAnimationAssets")));
		TestTrue(TEXT("database captures schema"), HasFieldContaining(Database->TypedSnapshot, TEXT("Schema")));
	}

	const FAnimDependency* Schema = FindDependency(TEXT("pose-search-schema"));
	TestNotNull(TEXT("database schema is recursively collected"), Schema);
	if (Database && Schema)
	{
		TestTrue(TEXT("database references the recursively collected schema"),
			Database->TypedSnapshot.ObjectReferences.Contains(Schema->ObjectPath));
	}
	if (Schema)
	{
		TestTrue(TEXT("schema has typed snapshot"), Schema->TypedSnapshot.bHasSnapshot);
		TestEqual(TEXT("schema snapshot kind"), Schema->TypedSnapshot.Kind, FString(TEXT("pose-search-schema")));
		TestTrue(TEXT("schema captures channels"), HasFieldContaining(Schema->TypedSnapshot, TEXT("Channels")));
		TestTrue(TEXT("schema captures skeletons"), HasFieldContaining(Schema->TypedSnapshot, TEXT("Skeletons")));
	}

	const TSharedPtr<FAnimGraphAST> SecondAST = FAnimBPExporter::ExportToAST(Blueprint);
	const FAnimDependency* SecondDatabase = SecondAST.IsValid() ? SecondAST->Dependencies.FindByPredicate([](const FAnimDependency& Dependency)
	{
		return Dependency.Role == TEXT("pose-search-database");
	}) : nullptr;
	TestNotNull(TEXT("second export contains database"), SecondDatabase);
	if (Database && SecondDatabase)
	{
		TestEqual(TEXT("typed asset hash is stable across exports"), SecondDatabase->TypedSnapshot.StableHash, Database->TypedSnapshot.StableHash);
	}

	int64 TotalTypedFields = 0;
	int64 TotalTypedCharacters = 0;
	int32 MaxTypedFieldCharacters = 0;
	for (const FAnimDependency& Dependency : AST->Dependencies)
	{
		for (const FExternalAssetSnapshotField& Field : Dependency.TypedSnapshot.Fields)
		{
			++TotalTypedFields;
			const int32 Characters = Field.Path.Len() + Field.Type.Len() + Field.Value.Len();
			TotalTypedCharacters += Characters;
			MaxTypedFieldCharacters = FMath::Max(MaxTypedFieldCharacters, Characters);
		}
	}
	AddInfo(FString::Printf(TEXT("TypedSnapshotMetrics fields=%lld characters=%lld max-field=%d dependencies=%d"),
		TotalTypedFields, TotalTypedCharacters, MaxTypedFieldCharacters, AST->Dependencies.Num()));
	UE_LOG(LogTemp, Display, TEXT("TypedSnapshotMetrics fields=%lld characters=%lld max-field=%d dependencies=%d"),
		TotalTypedFields, TotalTypedCharacters, MaxTypedFieldCharacters, AST->Dependencies.Num());

	TArray<FAnimLangParseError> ParseErrors;
	const TSharedPtr<FAnimGraphAST> Parsed = FAnimLangParser::Parse(AST->ToString(), ParseErrors);
	TestTrue(TEXT("real typed snapshot DSL parses"), Parsed.IsValid() && ParseErrors.IsEmpty());
	const FAnimDependency* ParsedDatabase = Parsed.IsValid() ? Parsed->Dependencies.FindByPredicate([](const FAnimDependency& Dependency)
	{
		return Dependency.Role == TEXT("pose-search-database");
	}) : nullptr;
	TestNotNull(TEXT("parsed DSL contains database"), ParsedDatabase);
	if (Database && ParsedDatabase)
	{
		TestEqual(TEXT("typed asset hash survives full DSL parse"), ParsedDatabase->TypedSnapshot.StableHash, Database->TypedSnapshot.StableHash);
		TestEqual(TEXT("typed field count survives full DSL parse"), ParsedDatabase->TypedSnapshot.Fields.Num(), Database->TypedSnapshot.Fields.Num());
	}
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
