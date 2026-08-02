// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"

#include "AnimLangDiagnostics.h"
#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "AnimLangParser.h"
#include "AnimLispWorkspace.h"
#include "AnimGraphNode_ControlRig.h"
#include "Animation/AnimBlueprint.h"
#if ENGINE_MAJOR_VERSION < 5
#include "ControlRigBlueprint.h"
#else
#if ENGINE_MAJOR_VERSION >= 5
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#endif
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RigLangExporter.h"
#include "RigLangImporter.h"
#include "Rigs/RigHierarchy.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace AnimLispBundleRoundTripTests
{
constexpr EAutomationTestFlags Flags =
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter;

FString RigSource()
{
	return TEXT("(rig-module :asset \"/Game/Rigs/BundleRig\" :class \"/Script/ControlRig.ControlRigBlueprint\" :version 1 :content-hash \"bundle-rig-hash\")\n")
		TEXT("(define-rig-entry \"ForwardsSolve\" :id \"entry-forwards\" :event \"Forwards Solve\")\n");
}

FString AnimSource()
{
	return TEXT("(anim-module :asset \"/Game/Animations/BundleAnim\" :class \"/Script/Engine.AnimBlueprint\" :version 1 :content-hash \"bundle-anim-hash\")\n")
		TEXT("(import-rig :asset \"/Game/Rigs/BundleRig\" :alias BundleRig :content-hash \"bundle-rig-hash\")\n")
		TEXT("(rig-entry :target \"BundleRig/ForwardsSolve\")\n");
}

FString LegacyControlRigAnimSource()
{
	TSharedPtr<FAnimGraphAST> AST = MakeShared<FAnimGraphAST>();
	AST->Name = TEXT("ABP_LegacyRigFallback");
	AST->RootNode = MakeShared<FAnimNodeAST>();
	AST->RootNode->NodeType = TEXT("control-rig");
	AST->RootNode->NodeClassPath = TEXT("/Script/ControlRigDeveloper.AnimGraphNode_ControlRig");
	AST->RootNode->Coverage = EAnimNodeCoverage::Reflected;
	AST->RootNode->Properties.Add(
		TEXT("control-rig-asset-reference"),
		TEXT("\"(BlueprintRigClass=\\\"/Script/ControlRig.ControlRigBlueprintGeneratedClass'/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement.CR_Biped_FootPlacement_C'\\\")\""));
	return AST->ToString();
}

UAnimGraphNode_ControlRig* FindControlRigNode(UAnimBlueprint* Blueprint)
{
	if (!Blueprint) return nullptr;
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UAnimGraphNode_ControlRig* ControlRigNode = Cast<UAnimGraphNode_ControlRig>(Node))
			{
				return ControlRigNode;
			}
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispBundlePreflightNoMutationTest,
	"AnimBP2FP.AnimLisp.BundleRoundTrip.PreflightNoMutation",
	AnimLispBundleRoundTripTests::Flags)

bool FAnimLispBundlePreflightNoMutationTest::RunTest(const FString& Parameters)
{
	auto AssertRejectedWithoutMutation = [this](
		const FString& CaseName,
		const TArray<FAnimLispBundleSource>& Sources)
	{
		if (!TestFalse(TEXT("sentinel fixture class is concrete"),
			UAnimBlueprint::StaticClass()->HasAnyClassFlags(CLASS_Abstract))) return false;
		const FString TargetPackageName = FString::Printf(
			TEXT("/Engine/Transient/AnimLispBundleSentinel_%s_%s"),
			*CaseName,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UPackage* TargetPackage = CreatePackage(*TargetPackageName);
		UAnimBlueprint* Sentinel = NewObject<UAnimBlueprint>(TargetPackage, TEXT("OriginalTargetSentinel"));
		FAnimLispBundleImportOptions Options;
		Options.Mode = EAnimLispBundleImportMode::Strict;
		Options.TargetRoot = TargetPackageName;
		const FAnimLispBundleImportResult Result = FAnimBPImporter::ImportBundle(Sources, Options);
		TestFalse(*FString::Printf(TEXT("%s is rejected"), *CaseName), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("%s starts no asset mutation"), *CaseName), Result.bMutationStarted);
		TestTrue(*FString::Printf(TEXT("%s preserves target package identity"), *CaseName),
			FindPackage(nullptr, *TargetPackageName) == TargetPackage);
		TestTrue(*FString::Printf(TEXT("%s preserves original target sentinel"), *CaseName),
			StaticFindObjectFast(UAnimBlueprint::StaticClass(), TargetPackage, TEXT("OriginalTargetSentinel")) == Sentinel);
		return !Result.bSuccess && !Result.bMutationStarted;
	};

	TArray<FAnimLispBundleSource> MissingRigSources;
	MissingRigSources.Add({TEXT("BundleAnim.animlang"), AnimLispBundleRoundTripTests::AnimSource()});
	if (!AssertRejectedWithoutMutation(TEXT("MissingRig"), MissingRigSources)) return false;

	TArray<FAnimLispBundleSource> StaleHashSources;
	StaleHashSources.Add({TEXT("BundleAnim.animlang"),
		AnimLispBundleRoundTripTests::AnimSource().Replace(TEXT("bundle-rig-hash"), TEXT("stale-rig-hash"))});
	StaleHashSources.Add({TEXT("BundleRig.riglang"), AnimLispBundleRoundTripTests::RigSource()});
	if (!AssertRejectedWithoutMutation(TEXT("StaleHash"), StaleHashSources)) return false;

	TArray<FAnimLispBundleSource> RigCompileFailureSources;
	RigCompileFailureSources.Add({TEXT("BundleAnim.animlang"), AnimLispBundleRoundTripTests::AnimSource()});
	RigCompileFailureSources.Add({TEXT("BundleRig.riglang"), AnimLispBundleRoundTripTests::RigSource()});
	FAnimLispBundleImportOptions RigFailureOptions;
	RigFailureOptions.Mode = EAnimLispBundleImportMode::Strict;
	RigFailureOptions.TargetRoot = TEXT("/Engine/Transient/AnimLispBundleRigFailure");
	const FAnimLispBundleImportResult RigFailure =
		FAnimBPImporter::ImportBundle(RigCompileFailureSources, RigFailureOptions);
	TestFalse(TEXT("non-executable Rig rejects the bundle"), RigFailure.bSuccess);
	TestFalse(TEXT("Rig gate failure does not begin target mutation"), RigFailure.bMutationStarted);
	TestEqual(TEXT("failed staging assets are not published"), RigFailure.StagedAssets.Num(), 0);
	return TestTrue(TEXT("failure is attributed to the Rig compile gate"),
		RigFailure.Diagnostics.ToReport().Contains(TEXT("compile"), ESearchCase::IgnoreCase));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispBundleStrictRejectsLegacyRigFallbackTest,
	"AnimBP2FP.AnimLisp.BundleRoundTrip.StrictRejectsLegacyRigFallback",
	AnimLispBundleRoundTripTests::Flags)

bool FAnimLispBundleStrictRejectsLegacyRigFallbackTest::RunTest(const FString& Parameters)
{
	FAnimLispBundleImportOptions Options;
	Options.Mode = EAnimLispBundleImportMode::Strict;
	Options.TargetRoot = TEXT("/Engine/Transient/AnimLispStrictLegacyFallback");
	const FAnimLispBundleImportResult Result = FAnimBPImporter::ImportBundle(
		{{TEXT("LegacyRigFallback.animlang"), AnimLispBundleRoundTripTests::LegacyControlRigAnimSource()}}, Options);
	TestFalse(TEXT("strict bundle rejects legacy Control Rig fallback"), Result.bSuccess);
	TestFalse(TEXT("strict rejection starts no target mutation"), Result.bMutationStarted);
	TestEqual(TEXT("strict rejection creates no staged assets"), Result.StagedAssets.Num(), 0);
	return TestTrue(TEXT("strict rejection has a dedicated legacy diagnostic"),
		Result.Diagnostics.ToReport().Contains(TEXT("legacy Control Rig"), ESearchCase::IgnoreCase));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispBundleBindingPreflightNoAnimStageTest,
	"AnimBP2FP.AnimLisp.BundleRoundTrip.BindingPreflightNoAnimStage",
	AnimLispBundleRoundTripTests::Flags)

bool FAnimLispBundleBindingPreflightNoAnimStageTest::RunTest(const FString& Parameters)
{
	FString AnimSource;
	FString RigSource;
	if (!TestTrue(TEXT("real Mover Anim fixture loads"), FFileHelper::LoadFileToString(
		AnimSource, *(FPaths::ProjectSavedDir() / TEXT("BP2DSL/AnimBP/Blueprints/SandboxCharacter_Mover_ABP.animlang"))))) return false;
	if (!TestTrue(TEXT("real foot Rig fixture loads"), FFileHelper::LoadFileToString(
		RigSource, *(FPaths::ProjectSavedDir() / TEXT("BP2DSL/Rig/Blueprints/ControlRigs/CR_Biped_FootPlacement.riglang"))))) return false;

	auto ExpectRejected = [this, &AnimSource, &RigSource](
		const FString& Label,
		const TFunctionRef<void(FAnimRigNodeBinding&)>& Mutate,
		const FString& ExpectedDiagnostic)
	{
		TArray<FAnimLangParseError> ParseErrors;
		TSharedPtr<FAnimGraphAST> AST = FAnimLangParser::Parse(AnimSource, ParseErrors);
		TSharedPtr<FAnimNodeAST> RigNode;
		if (AST)
		{
			AST->VisitNodes([&RigNode](const TSharedPtr<FAnimNodeAST>& Node)
			{
				if (!RigNode && Node.IsValid() && Node->RigBinding.IsSet()) RigNode = Node;
			});
		}
		if (!TestTrue(*(Label + TEXT(" parses a typed Control Rig fixture")),
			AST.IsValid() && RigNode.IsValid())) return false;
		Mutate(RigNode->RigBinding.GetValue());
		FAnimLispBundleImportOptions Options;
		Options.Mode = EAnimLispBundleImportMode::Strict;
		Options.TargetRoot = TEXT("/Engine/Transient/AnimLispBindingPreflight");
		const FAnimLispBundleImportResult Result = FAnimBPImporter::ImportBundle({
			{TEXT("Mover.animlang"), AST->ToString()},
			{TEXT("FootRig.riglang"), RigSource}}, Options);
		TestFalse(*(Label + TEXT(" rejects the bundle")), Result.bSuccess);
		TestEqual(*(Label + TEXT(" stages no assets")), Result.StagedAssets.Num(), 0);
		return TestTrue(*(Label + TEXT(" reports the exact binding problem")),
			Result.Diagnostics.ToReport().Contains(ExpectedDiagnostic, ESearchCase::IgnoreCase));
	};

	if (!ExpectRejected(TEXT("missing entry"), [](FAnimRigNodeBinding& Binding)
	{
		Binding.EntryName = TEXT("MissingEntry");
	}, TEXT("MissingEntry"))) return false;
	if (!ExpectRejected(TEXT("duplicate input"), [](FAnimRigNodeBinding& Binding)
	{
		if (Binding.Inputs.Num() != 0)
		{
			Binding.Inputs.Reserve(Binding.Inputs.Num() + 1);
			const FAnimRigInputBinding Duplicate = Binding.Inputs[0];
			Binding.Inputs.Add(Duplicate);
		}
	}, TEXT("duplicate"))) return false;
	return ExpectRejected(TEXT("wrong input type"), [](FAnimRigNodeBinding& Binding)
	{
		if (Binding.Inputs.Num() != 0)
		{
			Binding.Inputs[0].ResolvedType.CPPType = TEXT("FString");
			Binding.Inputs[0].ResolvedType.CPPTypeObject.Reset();
		}
	}, TEXT("type"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispBundleLegacyRigFallbackWarnsTest,
	"AnimBP2FP.AnimLisp.BundleRoundTrip.LegacyRigFallbackWarns",
	AnimLispBundleRoundTripTests::Flags)

bool FAnimLispBundleLegacyRigFallbackWarnsTest::RunTest(const FString& Parameters)
{
	FAnimLispBundleImportOptions Options;
	Options.Mode = EAnimLispBundleImportMode::Legacy;
	Options.TargetRoot = TEXT("/Engine/Transient/AnimLispLegacyRigFallback");
	const FAnimLispBundleImportResult Result = FAnimBPImporter::ImportBundle(
		{{TEXT("LegacyMover.animlang"), AnimLispBundleRoundTripTests::LegacyControlRigAnimSource()}}, Options);
	if (!TestTrue(TEXT("explicit Legacy mode imports against the existing Rig asset"), Result.bSuccess))
	{
		AddError(Result.Diagnostics.ToReport());
		return false;
	}
	TestFalse(TEXT("transient legacy staging does not mutate persistent targets"), Result.bMutationStarted);
	TestEqual(TEXT("legacy Anim-only bundle publishes one staged asset"), Result.StagedAssets.Num(), 1);
	TestTrue(TEXT("legacy staged asset is an AnimBlueprint"),
		Result.StagedAssets.Num() == 1 && Result.StagedAssets[0].IsA<UAnimBlueprint>());
	return TestTrue(TEXT("legacy fallback emits a non-exact coverage warning"),
		Result.Diagnostics.ToReport().Contains(TEXT("non-exact bundle coverage"), ESearchCase::IgnoreCase));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispBundleDependencyOrderTest,
	"AnimBP2FP.AnimLisp.BundleRoundTrip.DependencyOrder",
	AnimLispBundleRoundTripTests::Flags)

bool FAnimLispBundleDependencyOrderTest::RunTest(const FString& Parameters)
{
	FAnimLispWorkspace Workspace;
	Workspace.AddSource(TEXT("00-BundleAnim.animlang"), AnimLispBundleRoundTripTests::AnimSource());
	Workspace.AddSource(TEXT("99-BundleRig.riglang"), AnimLispBundleRoundTripTests::RigSource());
	FAnimLangDiagnostics Diagnostics;
	if (!TestTrue(TEXT("bundle workspace preflight succeeds"), Workspace.Build(Diagnostics)))
	{
		AddError(Diagnostics.ToReport());
		return false;
	}
	TArray<FAnimLispImportPlanEntry> Plan;
	if (!TestTrue(TEXT("workspace builds a dependency import plan"),
		Workspace.BuildImportPlan(Plan, Diagnostics))) return false;
	if (!TestEqual(TEXT("bundle plan contains both modules"), Plan.Num(), 2)) return false;
	TestEqual(TEXT("Rig is imported before Anim regardless of source insertion or filename order"),
		Plan[0].ModuleId.Kind, EAnimLispModuleKind::Rig);
	TestEqual(TEXT("Anim follows its Rig dependency"),
		Plan[1].ModuleId.Kind, EAnimLispModuleKind::Anim);
	TestEqual(TEXT("Rig plan entry preserves source identity"),
		Plan[0].SourceFile, FString(TEXT("99-BundleRig.riglang")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispRealRigStagesBeforeAnimAndDiffsTest,
	"AnimBP2FP.AnimLisp.BundleRoundTrip.RealRigStagesBeforeAnimAndDiffs",
	AnimLispBundleRoundTripTests::Flags)

bool FAnimLispRealRigStagesBeforeAnimAndDiffsTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* LiveAnim = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Blueprints/SandboxCharacter_Mover_ABP.SandboxCharacter_Mover_ABP"));
	UControlRigBlueprint* LiveRig = LoadObject<UControlRigBlueprint>(nullptr,
		TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement.CR_Biped_FootPlacement"));
	if (!TestNotNull(TEXT("live Mover Anim asset loads"), LiveAnim)
		|| !TestNotNull(TEXT("live foot Rig asset loads"), LiveRig)) return false;
	TMap<FString, FRigLangExportResult> LiveRigModules;
	const TSharedPtr<FAnimGraphAST> LiveAnimAST = FAnimBPExporter::ExportToAST(LiveAnim, &LiveRigModules);
	const FRigLangExportResult* LiveRigExport = LiveRigModules.Find(LiveRig->GetOutermost()->GetName());
	if (!TestTrue(TEXT("live Mover and referenced foot Rig export as one typed bundle"),
		LiveAnimAST.IsValid() && LiveRigExport && LiveRigExport->bSuccess && LiveRigExport->Module.IsValid())) return false;
	const FString AnimSource = LiveAnimAST->ToString();
	const FString RigSource = LiveRigExport->Module->ToCanonicalString();

	FAnimLispBundleImportOptions Options;
	Options.Mode = EAnimLispBundleImportMode::Strict;
	Options.TargetRoot = TEXT("/Engine/Transient/AnimLispRealBundle");
	const FAnimLispBundleImportResult Result = FAnimBPImporter::ImportBundle({
		{TEXT("00-Mover.animlang"), AnimSource},
		{TEXT("99-FootRig.riglang"), RigSource}}, Options);
	if (!TestTrue(TEXT("real strict bundle imports"), Result.bSuccess))
	{
		AddError(Result.Diagnostics.ToReport());
		return false;
	}
	if (!TestEqual(TEXT("real bundle plan has Rig and Anim"), Result.Plan.Num(), 2)
		|| !TestEqual(TEXT("real bundle stages Rig and Anim"), Result.StagedAssets.Num(), 2)) return false;
	TestEqual(TEXT("dependency plan puts Rig first"), Result.Plan[0].ModuleId.Kind, EAnimLispModuleKind::Rig);
	TestEqual(TEXT("dependency plan puts Anim second"), Result.Plan[1].ModuleId.Kind, EAnimLispModuleKind::Anim);
	UControlRigBlueprint* StagedRig = Cast<UControlRigBlueprint>(Result.StagedAssets[0]);
	UAnimBlueprint* StagedAnim = Cast<UAnimBlueprint>(Result.StagedAssets[1]);
	if (!TestNotNull(TEXT("first staged asset is ControlRigBlueprint"), StagedRig)
		|| !TestNotNull(TEXT("second staged asset is AnimBlueprint"), StagedAnim)) return false;
	TestNotNull(TEXT("staged Rig compiled generated class"), StagedRig->GeneratedClass.Get());
	TestNotNull(TEXT("staged Anim compiled generated class"), StagedAnim->GeneratedClass.Get());

	const TSharedPtr<const FRigModuleAST> SourceRig = Result.Plan[0].RigAST;
	const TSharedPtr<const FAnimGraphAST> SourceAnim = Result.Plan[1].AnimAST;
	if (!TestTrue(TEXT("plan preserves both parsed ASTs"), SourceRig.IsValid() && SourceAnim.IsValid())) return false;
	TestEqual(TEXT("foot Rig has one public forwards entry"), SourceRig->Entries.FilterByPredicate(
		[](const FRigEntryAST& Entry)
		{
			return Entry.Name == TEXT("ForwardsSolve")
				&& !Entry.EventName.Contains(TEXT("Construction"), ESearchCase::IgnoreCase);
		}).Num(), 1);
	int32 TypedRigNodeCount = 0;
	bool bAllEightInputsMatchPublicTypes = true;
	SourceAnim->VisitNodes([&](const TSharedPtr<FAnimNodeAST>& Node)
	{
		if (Node.IsValid() && Node->RigBinding.IsSet()
			&& Node->RigBinding->EntryName == TEXT("ForwardsSolve")
			&& Node->RigBinding->Inputs.Num() == 8)
		{
			++TypedRigNodeCount;
			for (const FAnimRigInputBinding& Input : Node->RigBinding->Inputs)
			{
				bAllEightInputsMatchPublicTypes &= SourceRig->Variables.FilterByPredicate(
					[&Input](const FRigVariableAST& Variable)
					{
						return Variable.Access == ERigVariableAccess::PublicInput
							&& AnimLispStableRuntimeSymbol(Variable.Name) == Input.RigInputName
							&& Variable.Type == Input.ResolvedType;
					}).Num() == 1;
			}
		}
	});
	TestEqual(TEXT("Mover has one typed foot Rig binding with eight inputs"), TypedRigNodeCount, 1);
	TestTrue(TEXT("all eight bindings match one exact public Rig variable/type"), bAllEightInputsMatchPublicTypes);

	const FRigLangExportResult ExportedRig = FRigLangExporter::Export(StagedRig);
	if (!TestTrue(TEXT("staged Rig re-exports exactly"), ExportedRig.bSuccess && ExportedRig.Module.IsValid())) return false;
	TestEqual(TEXT("Rig hierarchy/variables semantic diff is zero"),
		FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(*ExportedRig.Module, *SourceRig),
		FRigLangImporter::BuildHierarchyVariableSemanticSnapshot(*SourceRig, *SourceRig));
	TestEqual(TEXT("Rig graph semantic diff is zero"),
		FRigLangImporter::BuildGraphSemanticSnapshot(*ExportedRig.Module, *SourceRig),
		FRigLangImporter::BuildGraphSemanticSnapshot(*SourceRig, *SourceRig));

	// ImportBundle itself executes the complete Anim canonical re-export gate before publishing this asset.
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnimLispPersistentBundleCommitTest,
	"AnimBP2FP.AnimLisp.BundleRoundTrip.PersistentCommitUsesCommittedRig",
	AnimLispBundleRoundTripTests::Flags)

bool FAnimLispPersistentBundleCommitTest::RunTest(const FString& Parameters)
{
	UAnimBlueprint* LiveAnim = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Blueprints/SandboxCharacter_Mover_ABP.SandboxCharacter_Mover_ABP"));
	UControlRigBlueprint* LiveRig = LoadObject<UControlRigBlueprint>(nullptr,
		TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement.CR_Biped_FootPlacement"));
	if (!TestNotNull(TEXT("persistent fixture Anim loads"), LiveAnim)
		|| !TestNotNull(TEXT("persistent fixture Rig loads"), LiveRig)) return false;

	TMap<FString, FRigLangExportResult> RigModules;
	const TSharedPtr<FAnimGraphAST> AnimAST = FAnimBPExporter::ExportToAST(LiveAnim, &RigModules);
	const FRigLangExportResult* RigExport = RigModules.Find(LiveRig->GetOutermost()->GetName());
	if (!TestTrue(TEXT("persistent fixture exports a complete bundle"),
		AnimAST.IsValid() && RigExport && RigExport->bSuccess && RigExport->Module.IsValid())) return false;

	const FString TargetRoot = FString::Printf(TEXT("/Game/AnimBP2FP/Automation/Bundle_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString RigPackage = TargetRoot / FPaths::GetBaseFilename(RigExport->Module->Header.ModuleId.AssetPath);
	const FString AnimPackage = TargetRoot / AnimAST->Name;
	const FString RigFilename = FPackageName::LongPackageNameToFilename(
		RigPackage, FPackageName::GetAssetPackageExtension());
	const FString AnimFilename = FPackageName::LongPackageNameToFilename(
		AnimPackage, FPackageName::GetAssetPackageExtension());
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*RigFilename, false, true);
		IFileManager::Get().Delete(*AnimFilename, false, true);
	};

	FAnimLispBundleImportOptions Options;
	Options.Mode = EAnimLispBundleImportMode::Strict;
	Options.TargetRoot = TargetRoot;
	Options.bCommitPersistent = true;
	Options.TestFailSaveIndex = 1;
	const TArray<FAnimLispBundleSource> Sources = {
		{TEXT("00-Mover.animlang"), AnimAST->ToString()},
		{TEXT("99-FootRig.riglang"), RigExport->Module->ToCanonicalString()}};
	const FAnimLispBundleImportResult InjectedFailure = FAnimBPImporter::ImportBundle(Sources, Options);
	TestFalse(TEXT("injected second-package save failure rejects the commit"), InjectedFailure.bSuccess);
	TestTrue(TEXT("injected save failure records persistent mutation"), InjectedFailure.bMutationStarted);
	TestFalse(TEXT("rollback deletes the first saved package"), IFileManager::Get().FileExists(*RigFilename));
	TestFalse(TEXT("rollback deletes the failed save target"), IFileManager::Get().FileExists(*AnimFilename));
	TestNull(TEXT("rollback releases the Rig package identity"), FindPackage(nullptr, *RigPackage));
	TestNull(TEXT("rollback releases the Anim package identity"), FindPackage(nullptr, *AnimPackage));

	Options.TestFailSaveIndex = INDEX_NONE;
	const FAnimLispBundleImportResult Result = FAnimBPImporter::ImportBundle({
		{TEXT("00-Mover.animlang"), AnimAST->ToString()},
		{TEXT("99-FootRig.riglang"), RigExport->Module->ToCanonicalString()}}, Options);
	if (!TestTrue(TEXT("persistent strict bundle commits"), Result.bSuccess))
	{
		AddError(Result.Diagnostics.ToReport());
		return false;
	}
	TestTrue(TEXT("persistent commit records target mutation"), Result.bMutationStarted);
	if (!TestEqual(TEXT("persistent commit returns Rig and Anim"), Result.StagedAssets.Num(), 2)) return false;
	UControlRigBlueprint* CommittedRig = Cast<UControlRigBlueprint>(Result.StagedAssets[0]);
	UAnimBlueprint* CommittedAnim = Cast<UAnimBlueprint>(Result.StagedAssets[1]);
	if (!TestNotNull(TEXT("committed Rig is returned first"), CommittedRig)
		|| !TestNotNull(TEXT("committed Anim is returned second"), CommittedAnim)) return false;
	TestEqual(TEXT("committed Rig uses the target package"), CommittedRig->GetOutermost()->GetName(), RigPackage);
	TestEqual(TEXT("committed Anim uses its AST name"), CommittedAnim->GetOutermost()->GetName(), AnimPackage);
	TestNotNull(TEXT("committed Rig has a generated class"), CommittedRig->GeneratedClass.Get());
	TestNotNull(TEXT("committed Anim has a generated class"), CommittedAnim->GeneratedClass.Get());
	TestTrue(TEXT("committed Rig package was saved"), IFileManager::Get().FileExists(*RigFilename));
	TestTrue(TEXT("committed Anim package was saved"), IFileManager::Get().FileExists(*AnimFilename));

	UAnimGraphNode_ControlRig* ControlRigNode = AnimLispBundleRoundTripTests::FindControlRigNode(CommittedAnim);
	UControlRigBlueprint* BoundRig = ControlRigNode
		? Cast<UControlRigBlueprint>(ControlRigNode->Node.GetControlRigAssetReference().GetEditorAsset())
		: nullptr;
	if (!TestNotNull(TEXT("committed Anim contains a resolved Control Rig node"), ControlRigNode)
		|| !TestNotNull(TEXT("committed Control Rig node resolves an asset"), BoundRig)) return false;
	TestEqual(TEXT("committed Anim binds the committed Rig package"), BoundRig->GetOutermost()->GetName(), RigPackage);
	return TestFalse(TEXT("committed Anim has no transient Rig reference"),
		BoundRig->GetPathName().Contains(TEXT("/Engine/Transient")));
}

#endif

#endif // UE 5.8+