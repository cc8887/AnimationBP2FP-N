// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"

#include "AnimBP2FPExportCommandlet.h"
#include "AnimBPExporter.h"
#include "FBP2FPMappingRegistry.h"
#include "AnimLispLintCommandlet.h"
#include "RigLangExportCommandlet.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "RigLangExporter.h"
#include "Animation/AnimBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangCommandletArgumentsTest,
	"AnimBP2FP.Commandlet.Arguments",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FRigLangCommandletArgumentsTest::RunTest(const FString& Parameters)
{
	FString AssetPath;
	FString Error;
	TestTrue(TEXT("Rig export accepts the documented exact asset command"),
		AnimBP2FPCommandlets::ParseAssetPath(
			TEXT("-run=RigLangExport -AssetPath=/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement"),
			AssetPath, Error));
	TestEqual(TEXT("Rig export preserves the package path"), AssetPath,
		FString(TEXT("/Game/Blueprints/ControlRigs/CR_Biped_FootPlacement")));
	TestFalse(TEXT("Rig export rejects an invalid content root"),
		AnimBP2FPCommandlets::ParseAssetPath(
			TEXT("-run=RigLangExport -AssetPath=../../Engine/Bad"), AssetPath, Error));
	TestFalse(TEXT("Rig export rejects /GameEvil"),
		AnimBP2FPCommandlets::ParseAssetPath(TEXT("-AssetPath=/GameEvil/R"), AssetPath, Error));
	TestFalse(TEXT("Rig export rejects doubled separators"),
		AnimBP2FPCommandlets::ParseAssetPath(TEXT("-AssetPath=/Game//R"), AssetPath, Error));
	TestFalse(TEXT("Rig export rejects colon syntax"),
		AnimBP2FPCommandlets::ParseAssetPath(TEXT("-AssetPath=/Game/R:Bad"), AssetPath, Error));
	TestTrue(TEXT("mapped output accepts the BP2DSL root"),
		AnimBP2FPCommandlets::ValidateMappedOutputPath(
			FPaths::ProjectSavedDir() / TEXT("BP2DSL/Rig/R.riglang"), Error));
	TestFalse(TEXT("mapped output rejects a sibling prefix"),
		AnimBP2FPCommandlets::ValidateMappedOutputPath(
			FPaths::ProjectSavedDir() / TEXT("BP2DSLEvil/R.riglang"), Error));
	TestTrue(TEXT("AssetRoot accepts exactly /Game"),
		AnimBP2FPCommandlets::ValidateAssetRoot(TEXT("/Game"), Error));
	TestTrue(TEXT("AssetRoot accepts a valid /Game descendant"),
		AnimBP2FPCommandlets::ValidateAssetRoot(TEXT("/Game/Blueprints"), Error));
	TestFalse(TEXT("AssetRoot rejects /GameEvil"),
		AnimBP2FPCommandlets::ValidateAssetRoot(TEXT("/GameEvil"), Error));
	TestFalse(TEXT("AssetRoot rejects doubled separators"),
		AnimBP2FPCommandlets::ValidateAssetRoot(TEXT("/Game//Blueprints"), Error));
	TestFalse(TEXT("AssetRoot rejects colon syntax"),
		AnimBP2FPCommandlets::ValidateAssetRoot(TEXT("/Game/Blueprints:Bad"), Error));

	bool bIncludeRigModules = false;
	TestTrue(TEXT("Anim export accepts IncludeRigModules"),
		AnimBP2FPCommandlets::ParseAnimExportParams(
			TEXT("-run=AnimBP2FPExport -AssetPath=/Game/Blueprints/SandboxCharacter_Mover_ABP -IncludeRigModules"),
			AssetPath, bIncludeRigModules, Error));
	TestTrue(TEXT("Anim export records IncludeRigModules"), bIncludeRigModules);
#if ANIMBP2FP_HAS_ANIM_AUTHORING
	UAnimBlueprint* MoverAnimBP = LoadObject<UAnimBlueprint>(nullptr,
		TEXT("/Game/Blueprints/SandboxCharacter_Mover_ABP.SandboxCharacter_Mover_ABP"));
	if (MoverAnimBP)
	{
		FAnimBPExporter::FExportOptions Options;
		TMap<FString, FRigLangExportResult> RigCache;
		TSharedPtr<FAnimGraphAST> AST;
		const FString Source = FAnimBPExporter::ExportWithOptions(MoverAnimBP, Options, RigCache, &AST);
		TestFalse(TEXT("cache API export succeeds"), Source.TrimStart().StartsWith(TEXT("; Error:")));
		TestTrue(TEXT("cache API returns the typed Anim AST"), AST.IsValid());
		if (AST.IsValid())
		{
			TestEqual(TEXT("every typed Rig import has one cached export"), RigCache.Num(), AST->RigImports.Num());
			for (const FAnimLispImport& Import : AST->RigImports)
			{
				const FRigLangExportResult* Cached = RigCache.Find(Import.Target.AssetPath);
				TestTrue(TEXT("Rig import resolves from the same export pass"),
					Cached && Cached->bSuccess && Cached->Module.IsValid());
				if (Cached && Cached->Module.IsValid())
				{
					TestEqual(TEXT("cached semantic hash matches the typed import"),
						Cached->Module->Header.ContentHash, Import.ExpectedHash);
				}
			}
		}
	}
	else
	{
		AddInfo(TEXT("SKIPPED: cache API integration fixture is not installed"));
	}
#endif

	FString Workspace;
	const FString DocumentedWorkspace = FPaths::ProjectSavedDir() / TEXT("BP2DSL");
	TestTrue(TEXT("Lint accepts the documented workspace root"),
		AnimBP2FPCommandlets::ParseWorkspace(
			TEXT("-run=AnimLispLint -Workspace=\"") + DocumentedWorkspace + TEXT("\""), Workspace, Error));
	TestFalse(TEXT("Lint rejects a workspace outside Saved/BP2DSL"),
		AnimBP2FPCommandlets::ParseWorkspace(
			TEXT("-run=AnimLispLint -Workspace=F:/GASP/Content"), Workspace, Error));

	AddExpectedError(TEXT("Missing Control Rig asset: /Game/DefinitelyMissing/CR_Missing"),
		EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("missing Rig asset returns nonzero"),
		NewObject<URigLangExportCommandlet>()->Main(
			TEXT("-AssetPath=/Game/DefinitelyMissing/CR_Missing")) != 0);
	const FString EmptyWorkspace = FPaths::ProjectSavedDir()
		/ TEXT("BP2DSL/AutomationEmptyWorkspace");
	IFileManager::Get().MakeDirectory(*EmptyWorkspace, true);
	AddExpectedError(TEXT("Workspace contains no AnimLisp module sources"),
		EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("empty workspace returns one"),
		NewObject<UAnimLispLintCommandlet>()->Main(
			TEXT("-Workspace=\"") + EmptyWorkspace + TEXT("\"")), 1);
	const FString EmptyDiagnostics = EmptyWorkspace / TEXT("Diagnostics/animlisp-lint.json");
	FString EmptyJson;
	TestTrue(TEXT("empty workspace still writes diagnostics JSON"),
		FFileHelper::LoadFileToString(EmptyJson, *EmptyDiagnostics));
	TSharedPtr<FJsonObject> EmptyRoot;
	TestTrue(TEXT("empty workspace diagnostics JSON parses"),
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(EmptyJson), EmptyRoot)
		&& EmptyRoot.IsValid() && EmptyRoot->GetIntegerField(TEXT("error_count")) == 1);

	const FString ManifestPath = FPaths::ProjectSavedDir() / TEXT("BP2DSL/animlisp-bundle.json");
	FString OriginalManifest;
	const bool bHadOriginalManifest = FFileHelper::LoadFileToString(OriginalManifest, *ManifestPath);
	const FString SafeOldOutput = FPaths::ProjectSavedDir() / TEXT("BP2DSL/AutomationCleanup/Old.riglang");
	const FString MissingAnimOutput = FBP2FPMappingRegistry::BlueprintToDSLPath(
		TEXT("/Game/DefinitelyMissing/ABP_Missing"), TEXT("AnimBP"), TEXT(".animlang"));
	const FString MaliciousOutsideOutput = FPaths::ProjectSavedDir() / TEXT("AutomationMustSurvive/Outside.riglang");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SafeOldOutput), true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(MissingAnimOutput), true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(MaliciousOutsideOutput), true);
	TestTrue(TEXT("safe old output fixture writes"), FFileHelper::SaveStringToFile(TEXT("old"), *SafeOldOutput));
	TestTrue(TEXT("current Anim output fixture writes"), FFileHelper::SaveStringToFile(TEXT("old"), *MissingAnimOutput));
	TestTrue(TEXT("outside output fixture writes"), FFileHelper::SaveStringToFile(TEXT("outside"), *MaliciousOutsideOutput));
	FString SafeJsonPath = SafeOldOutput, OutsideJsonPath = MaliciousOutsideOutput;
	FPaths::NormalizeFilename(SafeJsonPath);
	FPaths::NormalizeFilename(OutsideJsonPath);
	const FString CleanupManifest = FString::Printf(
		TEXT("{\"modules\":[{\"output_path\":\"%s\"},{\"output_path\":\"%s\"}]}"),
		*SafeJsonPath, *OutsideJsonPath);
	TestTrue(TEXT("old manifest fixture writes"), FFileHelper::SaveStringToFile(CleanupManifest, *ManifestPath));
	TestEqual(TEXT("missing bundle asset fails"), NewObject<UAnimBP2FPExportCommandlet>()->Main(
		TEXT("-AssetPath=/Game/DefinitelyMissing/ABP_Missing -IncludeRigModules")), 1);
	TestFalse(TEXT("missing asset cleanup revokes old safe manifest output"), FPaths::FileExists(SafeOldOutput));
	TestFalse(TEXT("missing asset cleanup revokes current Anim target"), FPaths::FileExists(MissingAnimOutput));
	TestFalse(TEXT("missing asset cleanup revokes old manifest"), FPaths::FileExists(ManifestPath));
	TestTrue(TEXT("malicious manifest path outside root survives"), FPaths::FileExists(MaliciousOutsideOutput));

	const FString SemanticOld = FPaths::ProjectSavedDir() / TEXT("BP2DSL/AutomationCleanup/SemanticOld.riglang");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SemanticOld), true);
	FFileHelper::SaveStringToFile(TEXT("old"), *SemanticOld);
	TSet<FString> SemanticCleanup;
	SemanticCleanup.Add(SemanticOld);
	AnimBP2FPCommandlets::CleanupBundleTargets(SemanticCleanup);
	TestFalse(TEXT("semantic early failure uses the common cleanup"), FPaths::FileExists(SemanticOld));
	if (bHadOriginalManifest) FFileHelper::SaveStringToFile(OriginalManifest, *ManifestPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangCommandletAtomicOutputTest,
	"AnimBP2FP.Commandlet.AtomicOutput",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FRigLangCommandletAtomicOutputTest::RunTest(const FString& Parameters)
{
	const FString TempDir = FPaths::ProjectSavedDir() / TEXT("Automation/AnimBP2FPCommandlet");
	const FString OutputPath = TempDir / TEXT("Result.riglang");
	IFileManager::Get().MakeDirectory(*TempDir, true);
	TestTrue(TEXT("old successful output fixture is written"),
		FFileHelper::SaveStringToFile(TEXT("old-success"), *OutputPath));
	FString Error;
	TestTrue(TEXT("begin export removes the old successful output"),
		AnimBP2FPCommandlets::PrepareOutputForExport(OutputPath, Error));
	TestFalse(TEXT("failed export cannot leave the old successful output current"),
		FPaths::FileExists(OutputPath));
	TestTrue(TEXT("atomic staging commit succeeds"),
		AnimBP2FPCommandlets::WriteFileAtomically(OutputPath, TEXT("new-success"), Error));
	FString Written;
	TestTrue(TEXT("atomic output is readable"),
		FFileHelper::LoadFileToString(Written, *OutputPath));
	TestEqual(TEXT("atomic output contains only the committed content"), Written, FString(TEXT("new-success")));

	TestFalse(TEXT("duplicate output paths are rejected"),
		AnimBP2FPCommandlets::ValidateUniqueOutputPaths(
			{OutputPath, FPaths::ConvertRelativePathToFull(OutputPath)}, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigLangCommandletJsonContractsTest,
	"AnimBP2FP.Commandlet.JsonContracts",
	ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

bool FRigLangCommandletJsonContractsTest::RunTest(const FString& Parameters)
{
	const FString ValidManifest = TEXT(
		"{\"modules\":["
		"{\"module_identity\":\"anim:/Game/A\",\"source_asset\":\"/Game/A\","
		"\"output_path\":\"F:/Out/A.animlang\",\"canonical_hash\":\"sha256:a\","
		"\"dependencies\":[\"/Game/R\"],\"status\":\"success\","
		"\"coverage\":{\"exact\":1,\"reflected\":0,\"lossy\":0,\"unsupported\":0}},"
		"{\"module_identity\":\"rig:/Game/R\",\"source_asset\":\"/Game/R\","
		"\"output_path\":\"F:/Out/R.riglang\",\"canonical_hash\":\"sha256:r\","
		"\"dependencies\":[],\"status\":\"success\","
		"\"coverage\":{\"exact\":2,\"reflected\":0,\"lossy\":0,\"unsupported\":0}}]}");
	FString Error;
	TestTrue(TEXT("manifest parses with every required per-module field"),
		AnimBP2FPCommandlets::ValidateBundleManifestJson(ValidManifest, Error));
	TestFalse(TEXT("manifest rejects a missing coverage object"),
		AnimBP2FPCommandlets::ValidateBundleManifestJson(
			ValidManifest.Replace(TEXT(",\"coverage\":{\"exact\":2,\"reflected\":0,\"lossy\":0,\"unsupported\":0}"), TEXT("")), Error));
	TestFalse(TEXT("manifest rejects duplicate canonical output paths"),
		AnimBP2FPCommandlets::ValidateBundleManifestJson(
			ValidManifest.Replace(TEXT("F:/Out/R.riglang"), TEXT("F:/Out/A.animlang")), Error));

	const FString HashDir = FPaths::ProjectSavedDir() / TEXT("Automation/AnimBP2FPManifestHashes");
	const FString AnimPath = HashDir / TEXT("A.animlang");
	const FString RigPath = HashDir / TEXT("R.riglang");
	const FString AnimSource = TEXT("(anim-blueprint \"A\" :skeleton \"\")");
	FRigModuleAST HashRigModule;
	HashRigModule.Header.ModuleId = FAnimLispModuleId::FromAssetPath(TEXT("/Game/R"), EAnimLispModuleKind::Rig);
	HashRigModule.Header.AssetClassPath = TEXT("/Script/ControlRig.ControlRigBlueprint");
	HashRigModule.Header.Version = 1;
	HashRigModule.Header.ContentHash = FRigLangExporter::ComputeContentHash(
		HashRigModule.ToCanonicalHashInput());
	const FString RigHash = HashRigModule.Header.ContentHash;
	const FString RigSource = HashRigModule.ToCanonicalString();
	TestTrue(TEXT("hash fixture Anim output writes"),
		AnimBP2FPCommandlets::WriteFileAtomically(AnimPath, AnimSource, Error));
	TestTrue(TEXT("hash fixture Rig output writes"),
		AnimBP2FPCommandlets::WriteFileAtomically(RigPath, RigSource, Error));
	FString AnimJsonPath = AnimPath;
	FString RigJsonPath = RigPath;
	FPaths::NormalizeFilename(AnimJsonPath);
	FPaths::NormalizeFilename(RigJsonPath);
	const FString HashManifest = FString::Printf(
		TEXT("{\"modules\":[")
		TEXT("{\"module_identity\":\"anim:/Game/A\",\"source_asset\":\"/Game/A\",\"output_path\":\"%s\",")
		TEXT("\"canonical_hash\":\"%s\",\"dependencies\":[],\"status\":\"success\",\"coverage\":{\"exact\":1,\"reflected\":0,\"lossy\":0,\"unsupported\":0}},")
		TEXT("{\"module_identity\":\"rig:/Game/R\",\"source_asset\":\"/Game/R\",\"output_path\":\"%s\",")
		TEXT("\"canonical_hash\":\"%s\",\"dependencies\":[],\"status\":\"success\",\"coverage\":{\"exact\":1,\"reflected\":0,\"lossy\":0,\"unsupported\":0}}]}"),
		*AnimJsonPath, *FRigLangExporter::ComputeContentHash(AnimSource), *RigJsonPath, *RigHash);
	TestTrue(TEXT("manifest hashes match Anim canonical UTF-8 and Rig semantic header"),
		AnimBP2FPCommandlets::ValidateBundleOutputHashes(HashManifest, Error));
	TestTrue(TEXT("tampered Anim fixture writes"),
		AnimBP2FPCommandlets::WriteFileAtomically(AnimPath, AnimSource + TEXT(" "), Error));
	TestFalse(TEXT("manifest rejects a tampered canonical Anim output"),
		AnimBP2FPCommandlets::ValidateBundleOutputHashes(HashManifest, Error));
	TestTrue(TEXT("restore matching Anim fixture"),
		AnimBP2FPCommandlets::WriteFileAtomically(AnimPath, AnimSource, Error));
	TestTrue(TEXT("tampered Rig body fixture writes"),
		AnimBP2FPCommandlets::WriteFileAtomically(
			RigPath, RigSource + TEXT("(define-rig-entry \"Changed\" :id \"changed\" :event \"Changed\")\n"), Error));
	TestFalse(TEXT("manifest rejects Rig semantic body tampering"),
		AnimBP2FPCommandlets::ValidateBundleOutputHashes(HashManifest, Error));

	const FString TransactionDir = FPaths::ProjectSavedDir() / TEXT("Automation/AnimBP2FPBundleTransaction");
	const FString FirstTarget = TransactionDir / TEXT("First.animlang");
	const FString SecondParentFile = TransactionDir / TEXT("blocked-parent");
	const FString SecondTarget = SecondParentFile / TEXT("Second.riglang");
	const FString TransactionManifest = TransactionDir / TEXT("manifest.json");
	IFileManager::Get().MakeDirectory(*TransactionDir, true);
	FFileHelper::SaveStringToFile(TEXT("old-first"), *FirstTarget);
	FFileHelper::SaveStringToFile(TEXT("not-a-directory"), *SecondParentFile);
	FFileHelper::SaveStringToFile(TEXT("old-manifest"), *TransactionManifest);
	FString FirstJsonPath = FirstTarget;
	FString SecondJsonPath = SecondTarget;
	FPaths::NormalizeFilename(FirstJsonPath);
	FPaths::NormalizeFilename(SecondJsonPath);
	const FString TransactionManifestContent = FString::Printf(
		TEXT("{\"modules\":[")
		TEXT("{\"module_identity\":\"anim:/Game/A\",\"source_asset\":\"/Game/A\",\"output_path\":\"%s\",")
		TEXT("\"canonical_hash\":\"%s\",\"dependencies\":[],\"status\":\"success\",\"coverage\":{\"exact\":1,\"reflected\":0,\"lossy\":0,\"unsupported\":0}},")
		TEXT("{\"module_identity\":\"rig:/Game/R\",\"source_asset\":\"/Game/R\",\"output_path\":\"%s\",")
		TEXT("\"canonical_hash\":\"%s\",\"dependencies\":[],\"status\":\"success\",\"coverage\":{\"exact\":1,\"reflected\":0,\"lossy\":0,\"unsupported\":0}}]}"),
		*FirstJsonPath, *FRigLangExporter::ComputeContentHash(AnimSource), *SecondJsonPath, *RigHash);
	TestFalse(TEXT("second bundle staging failure returns false"),
		AnimBP2FPCommandlets::CommitBundleAtomically(
			{{FirstTarget, AnimSource}, {SecondTarget, RigSource}},
			TransactionManifest, TransactionManifestContent, Error));
	TestFalse(TEXT("bundle failure revokes the first target"), FPaths::FileExists(FirstTarget));
	TestFalse(TEXT("bundle failure revokes the old manifest"), FPaths::FileExists(TransactionManifest));

	const FString SuccessDir = FPaths::ProjectSavedDir() / TEXT("BP2DSL/AutomationBundleSuccess");
	const FString CurrentTarget = SuccessDir / TEXT("Current.animlang");
	const FString StaleSafeTarget = SuccessDir / TEXT("Stale.riglang");
	const FString SuccessManifest = SuccessDir / TEXT("manifest.json");
	const FString OutsideTarget = FPaths::ProjectSavedDir() / TEXT("AutomationMustSurvive/StaleOutside.riglang");
	IFileManager::Get().MakeDirectory(*SuccessDir, true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutsideTarget), true);
	FFileHelper::SaveStringToFile(TEXT("old-current"), *CurrentTarget);
	FFileHelper::SaveStringToFile(TEXT("old-stale"), *StaleSafeTarget);
	FFileHelper::SaveStringToFile(TEXT("outside"), *OutsideTarget);
	FFileHelper::SaveStringToFile(TEXT("old-manifest"), *SuccessManifest);
	FString CurrentJsonPath = CurrentTarget;
	FPaths::NormalizeFilename(CurrentJsonPath);
	const FString SuccessManifestContent = FString::Printf(
		TEXT("{\"modules\":[{\"module_identity\":\"anim:/Game/Current\",")
		TEXT("\"source_asset\":\"/Game/Current\",\"output_path\":\"%s\",")
		TEXT("\"canonical_hash\":\"%s\",\"dependencies\":[],\"status\":\"success\",")
		TEXT("\"coverage\":{\"exact\":1,\"reflected\":0,\"lossy\":0,\"unsupported\":0}}]}"),
		*CurrentJsonPath, *FRigLangExporter::ComputeContentHash(AnimSource));
	const TSet<FString> PriorTargets = {CurrentTarget, StaleSafeTarget, SuccessManifest, OutsideTarget};
	TestTrue(TEXT("successful bundle transaction commits with prior targets"),
		AnimBP2FPCommandlets::CommitBundleAtomically(
			{{CurrentTarget, AnimSource}}, SuccessManifest, SuccessManifestContent, PriorTargets, Error));
	TestFalse(TEXT("successful bundle removes stale safe Rig output"), FPaths::FileExists(StaleSafeTarget));
	TestTrue(TEXT("successful bundle preserves its current output"), FPaths::FileExists(CurrentTarget));
	TestTrue(TEXT("successful bundle preserves its committed manifest"), FPaths::FileExists(SuccessManifest));
	TestTrue(TEXT("successful bundle never removes an outside prior target"), FPaths::FileExists(OutsideTarget));

	FAnimLangDiagnostics CoverageDiagnostics;
	AnimBP2FPCommandlets::CollectStrictCoverageDiagnostics(
		TEXT("Lossy.animlang"),
		TEXT("(anim-blueprint \"A\" :skeleton \"\" (identity-pose :coverage lossy))"),
		CoverageDiagnostics);
	TestTrue(TEXT("parsed Anim lossy coverage is fatal"), CoverageDiagnostics.HasErrors());
	CoverageDiagnostics.Items.Reset();
	AnimBP2FPCommandlets::CollectStrictCoverageDiagnostics(
		TEXT("LegacyWarning.animlang"),
		TEXT("(anim-blueprint \"Legacy\" :anim-graph (control-rig ")
		TEXT(":control-rig-asset-reference \"/Game/Test/FootRig.FootRig_C\"))"),
		CoverageDiagnostics);
	TestEqual(TEXT("Anim parser warning is retained as a warning"),
		CoverageDiagnostics.WarningCount(), 1);
	TestEqual(TEXT("Lossy coverage is reported separately as one error"),
		CoverageDiagnostics.ErrorCount(), 1);
	CoverageDiagnostics.Items.Reset();
	FRigModuleAST CoverageModule;
	CoverageModule.Header.ModuleId = FAnimLispModuleId::FromAssetPath(
		TEXT("/Game/Coverage"), EAnimLispModuleKind::Rig);
	CoverageModule.Header.AssetClassPath = TEXT("/Script/ControlRig.ControlRigBlueprint");
	CoverageModule.Header.Version = 1;
	FRigGraphAST& CoverageGraph = CoverageModule.Graphs.AddDefaulted_GetRef();
	CoverageGraph.StableId = TEXT("root");
	CoverageGraph.EditorGuid = TEXT("11111111-1111-1111-1111-111111111111");
	CoverageGraph.Role = TEXT("root");
	FRigNodeAST& UnsupportedNode = CoverageGraph.Nodes.AddDefaulted_GetRef();
	UnsupportedNode.Kind = ERigNodeKind::Comment;
	UnsupportedNode.Properties.Add(TEXT("comment-text"), TEXT("unsupported coverage fixture"));
	UnsupportedNode.StableId = TEXT("unsupported");
	UnsupportedNode.Guid = TEXT("22222222-2222-2222-2222-222222222222");
	UnsupportedNode.Coverage = ERigNodeCoverage::Unsupported;
	CoverageModule.Header.ContentHash = FRigLangExporter::ComputeContentHash(
		CoverageModule.ToCanonicalHashInput());
	AnimBP2FPCommandlets::CollectStrictCoverageDiagnostics(
		TEXT("Unsupported.riglang"),
		CoverageModule.ToCanonicalString(),
		CoverageDiagnostics);
	TestTrue(TEXT("parsed Rig unsupported coverage is fatal"), CoverageDiagnostics.HasErrors());
	CoverageDiagnostics.Items.Reset();
	FRigModuleAST HashMismatchModule;
	HashMismatchModule.Header.ModuleId = FAnimLispModuleId::FromAssetPath(
		TEXT("/Game/HashMismatch"), EAnimLispModuleKind::Rig);
	HashMismatchModule.Header.AssetClassPath = TEXT("/Script/ControlRig.ControlRigBlueprint");
	HashMismatchModule.Header.Version = 1;
	HashMismatchModule.Header.ContentHash = TEXT("sha256:wrong");
	AnimBP2FPCommandlets::CollectStrictCoverageDiagnostics(
		TEXT("HashMismatch.riglang"), HashMismatchModule.ToCanonicalString(), CoverageDiagnostics);
	TestTrue(TEXT("Rig lint rejects header versus semantic hash mismatch"), CoverageDiagnostics.HasErrors());

	FAnimLangDiagnostics WarningOnly;
	FAnimLangSourceLoc WarningLocation;
	WarningLocation.SourceFile = TEXT("Mover.animlang");
	WarningLocation.Line = 12;
	WarningLocation.Column = 7;
	WarningOnly.Add(EAnimLangDiagSeverity::Warning, EAnimLangDiagCategory::Capability,
		TEXT("warning-only fixture"), WarningLocation);
	TestEqual(TEXT("warning-only lint returns zero"),
		AnimBP2FPCommandlets::LintExitCode(WarningOnly), 0);
	const FString DiagnosticsJson = AnimBP2FPCommandlets::SerializeLintDiagnostics(
		TEXT("F:/Workspace"), 2, WarningOnly);
	TSharedPtr<FJsonObject> DiagnosticsRoot;
	TestTrue(TEXT("lint diagnostics JSON parses structurally"),
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DiagnosticsJson), DiagnosticsRoot)
		&& DiagnosticsRoot.IsValid());
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("lint JSON contains its diagnostic array"),
		DiagnosticsRoot.IsValid() && DiagnosticsRoot->TryGetArrayField(TEXT("diagnostics"), Items)
		&& Items && Items->Num() == 1);
	if (Items && Items->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
		TestEqual(TEXT("lint JSON severity is exact"), Item->GetStringField(TEXT("severity")), FString(TEXT("warning")));
		TestEqual(TEXT("lint JSON category is exact"), Item->GetStringField(TEXT("category")), FString(TEXT("capability")));
		TestEqual(TEXT("lint JSON source is exact"), Item->GetStringField(TEXT("source")), FString(TEXT("Mover.animlang")));
		TestEqual(TEXT("lint JSON line span is exact"), Item->GetIntegerField(TEXT("line")), 12);
		TestEqual(TEXT("lint JSON column span is exact"), Item->GetIntegerField(TEXT("column")), 7);
	}
	FAnimLangDiagnostics Fatal;
	Fatal.Add(EAnimLangDiagSeverity::Error, EAnimLangDiagCategory::Module,
		TEXT("fatal workspace fixture"));
	TestEqual(TEXT("fatal workspace lint returns one"),
		AnimBP2FPCommandlets::LintExitCode(Fatal), 1);
	return true;
}

#endif
