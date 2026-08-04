// AnimBP2FPExportCommandlet.cpp - Commandlet Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPExportCommandlet.h"
#include "AnimBPExporter.h"
#include "AnimLangAST.h"
#include "RigLangExporter.h"
#include "RigLangExportCommandlet.h"
#include "FBP2FPMappingRegistry.h"
#if ENGINE_MAJOR_VERSION < 5
#include "ControlRigBlueprint.h"
#else
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#include "ControlRigBlueprintLegacy.h"
#else
#include "ControlRigBlueprint.h"
#endif
#endif
#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UAnimBP2FPExportCommandlet::UAnimBP2FPExportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UAnimBP2FPExportCommandlet::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("=== AnimBP2FP Export Commandlet Starting ==="));
	
	// Output directory: unified convention {Project}/Saved/BP2DSL/AnimBP
	FString OutputDir = FPaths::ProjectDir() / TEXT("Saved") / TEXT("BP2DSL") / TEXT("AnimBP");
	if (!IFileManager::Get().DirectoryExists(*OutputDir))
	{
		IFileManager::Get().MakeDirectory(*OutputDir, true);
	}
	
	// Find all AnimBlueprints via AssetRegistry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// Ensure asset registry is fully loaded
	AssetRegistry.SearchAllAssets(true);
	
	FString AssetRoot = TEXT("/Game");
	FString ExactAssetPath;
	FParse::Value(*Params, TEXT("AssetRoot="), AssetRoot);
	FParse::Value(*Params, TEXT("AssetPath="), ExactAssetPath);
	const bool bIncludeRigModules = FParse::Param(*Params, TEXT("IncludeRigModules"));
	const FString ManifestPath = FPaths::ProjectSavedDir() / TEXT("BP2DSL/animlisp-bundle.json");
	TSet<FString> BundleRevokeSet;
	auto FailureCleanup = [&]()
	{
		if (bIncludeRigModules)
		{
			AnimBP2FPCommandlets::CleanupBundleTargets(BundleRevokeSet);
		}
	};
	FString ParameterError;
	if (bIncludeRigModules)
	{
		AnimBP2FPCommandlets::CollectSafeBundleRevokePaths(
			ManifestPath, FString(), BundleRevokeSet);
		bool bParsedInclude = false;
		if (!AnimBP2FPCommandlets::ParseAnimExportParams(
			Params, ExactAssetPath, bParsedInclude, ParameterError))
		{
			FailureCleanup();
			UE_LOG(LogTemp, Error, TEXT("%s"), *ParameterError);
			return 1;
		}
		const FString CurrentAnimTarget = FBP2FPMappingRegistry::BlueprintToDSLPath(
			ExactAssetPath, TEXT("AnimBP"), TEXT(".animlang"));
		if (!AnimBP2FPCommandlets::ValidateMappedOutputPath(CurrentAnimTarget, ParameterError))
		{
			FailureCleanup();
			UE_LOG(LogTemp, Error, TEXT("%s"), *ParameterError);
			return 1;
		}
		AnimBP2FPCommandlets::CollectSafeBundleRevokePaths(
			ManifestPath, CurrentAnimTarget, BundleRevokeSet);
	}
	const bool bAllowUnsupported = FParse::Param(*Params, TEXT("AllowUnsupported"));
	if (!AnimBP2FPCommandlets::ValidateAssetRoot(AssetRoot, ParameterError))
	{
		FailureCleanup();
		UE_LOG(LogTemp, Error, TEXT("%s"), *ParameterError);
		return 1;
	}

	TArray<FAssetData> AnimBPAssets;
	FARFilter Filter;
#if ENGINE_MAJOR_VERSION < 5
	Filter.ClassNames.Add(UAnimBlueprint::StaticClass()->GetFName());
#else
	Filter.ClassPaths.Add(UAnimBlueprint::StaticClass()->GetClassPathName());
#endif
	Filter.PackagePaths.Add(FName(*AssetRoot));
	Filter.bRecursivePaths = true;
	AssetRegistry.GetAssets(Filter, AnimBPAssets);
	if (!ExactAssetPath.IsEmpty())
	{
		AnimBPAssets.RemoveAll([&ExactAssetPath](const FAssetData& AssetData)
		{
#if ENGINE_MAJOR_VERSION < 5
			return AssetData.PackageName.ToString() != ExactAssetPath
				&& AssetData.ObjectPath.ToString() != ExactAssetPath;
#else
			return AssetData.PackageName.ToString() != ExactAssetPath
				&& AssetData.GetObjectPathString() != ExactAssetPath;
#endif
		});
	}
	
	UE_LOG(LogTemp, Log, TEXT("Found %d Animation Blueprints"), AnimBPAssets.Num());
	
	if (AnimBPAssets.Num() == 0)
	{
		FailureCleanup();
		UE_LOG(LogTemp, Warning, TEXT("No Animation Blueprints found!"));
		return 1;
	}
	
	int32 SuccessCount = 0;
	int32 FailCount = 0;
	FString AllOutput;
	TArray<TSharedPtr<FJsonValue>> ManifestModules;
	TArray<AnimBP2FPCommandlets::FBundleOutput> BundleOutputs;
	
	for (const FAssetData& AssetData : AnimBPAssets)
	{
		FString AssetName = AssetData.AssetName.ToString();
		FString PackagePath = AssetData.PackageName.ToString();
#if ENGINE_MAJOR_VERSION < 5
		FString ObjectPath = AssetData.ObjectPath.ToString();
#else
		FString ObjectPath = AssetData.GetObjectPathString();
#endif
		
		UE_LOG(LogTemp, Log, TEXT("Processing: %s (%s)"), *AssetName, *PackagePath);
		
		// Load the animation blueprint
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(AssetData.GetAsset());
		if (!AnimBP)
		{
			UE_LOG(LogTemp, Warning, TEXT("  FAILED to load: %s"), *AssetName);
			FailCount++;
			FailureCleanup();
			if (bIncludeRigModules) return 1;
			continue;
		}
		
		// Export
		FAnimBPExporter::FExportOptions Options;
		Options.bPrettyPrint = true;
		Options.IndentSize = 2;
		
		TMap<FString, FRigLangExportResult> RigExportCache;
		TSharedPtr<FAnimGraphAST> AST;
		FString DSLOutput = FAnimBPExporter::ExportWithOptions(
			AnimBP, Options, RigExportCache, &AST);
		const bool bExportError = DSLOutput.TrimStart().StartsWith(TEXT("; Error:"));
		const bool bUnsupported = DSLOutput.Contains(TEXT(":coverage unsupported"));
		if (bExportError || (bUnsupported && !bAllowUnsupported))
		{
			UE_LOG(LogTemp, Error, TEXT("  FAILED semantic export: %s%s"), *AssetName,
				bUnsupported ? TEXT(" (contains unsupported animation-node semantics; use -AllowUnsupported only for diagnostics)") : TEXT(""));
			FailCount++;
			FailureCleanup();
			if (bIncludeRigModules) return 1;
			continue;
		}
		
		// File header
		const FString FileContent = DSLOutput;
		FString WriteError;
		
		// Use unified path convention via mapping registry
		FString FullPath = AnimBP->GetPathName();
		FString OutputFilePath = FBP2FPMappingRegistry::BlueprintToDSLPath(FullPath, TEXT("AnimBP"), TEXT(".animlang"));
		if (!AnimBP2FPCommandlets::ValidateMappedOutputPath(OutputFilePath, WriteError))
		{
			UE_LOG(LogTemp, Warning, TEXT("  Cannot resolve safe DSL path for %s: %s"), *AssetName, *WriteError);
			FailCount++;
			FailureCleanup();
			if (bIncludeRigModules) return 1;
			continue;
		}

		// Ensure directory exists
		FString Dir = FPaths::GetPath(OutputFilePath);
		if (!IFileManager::Get().DirectoryExists(*Dir))
		{
			IFileManager::Get().MakeDirectory(*Dir, true);
		}

		if (bIncludeRigModules)
		{
			BundleOutputs.Add({OutputFilePath, FileContent});
			BundleRevokeSet.Add(FPaths::ConvertRelativePathToFull(OutputFilePath));
			++SuccessCount;
		}
		else if (AnimBP2FPCommandlets::PrepareOutputForExport(OutputFilePath, WriteError)
			&& AnimBP2FPCommandlets::WriteFileAtomically(OutputFilePath, FileContent, WriteError))
		{
			UE_LOG(LogTemp, Log, TEXT("  OK -> %s"), *OutputFilePath);
			SuccessCount++;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("  FAILED to write: %s"), *OutputFilePath);
			FailCount++;
			UE_LOG(LogTemp, Error, TEXT("  %s"), *WriteError);
			FailureCleanup();
			if (bIncludeRigModules) return 1;
			continue;
		}

		if (bIncludeRigModules)
		{
			if (!AST.IsValid())
			{
				UE_LOG(LogTemp, Error, TEXT("  FAILED to inspect typed Rig imports: %s"), *AssetName);
				++FailCount;
				FailureCleanup();
				if (bIncludeRigModules) return 1;
				continue;
			}
			TSharedRef<FJsonObject> AnimEntry = MakeShared<FJsonObject>();
			AnimEntry->SetStringField(TEXT("module_identity"), TEXT("anim:") + PackagePath);
			AnimEntry->SetStringField(TEXT("source_asset"), PackagePath);
			AnimEntry->SetStringField(TEXT("output_path"), OutputFilePath);
			AnimEntry->SetStringField(
				TEXT("canonical_hash"), FRigLangExporter::ComputeContentHash(DSLOutput));
			AnimEntry->SetStringField(TEXT("status"), TEXT("success"));
			TSet<FString> DependencySet;
			for (const FAnimDependency& Dependency : AST->Dependencies)
			{
				DependencySet.Add(Dependency.ObjectPath);
			}
			for (const FAnimLispImport& Import : AST->RigImports)
			{
				DependencySet.Add(Import.Target.ToString());
			}
			TArray<FString> SortedDependencies = DependencySet.Array();
			SortedDependencies.Sort();
			TArray<TSharedPtr<FJsonValue>> Dependencies;
			for (const FString& Dependency : SortedDependencies)
			{
				Dependencies.Add(MakeShared<FJsonValueString>(Dependency));
			}
			AnimEntry->SetArrayField(TEXT("dependencies"), Dependencies);
			int32 Exact = 0, Reflected = 0, Lossy = 0, Unsupported = 0;
			AST->VisitNodes([&](const TSharedPtr<FAnimNodeAST>& Node)
			{
				switch (Node->Coverage)
				{
				case EAnimNodeCoverage::Exact: ++Exact; break;
				case EAnimNodeCoverage::Reflected: ++Reflected; break;
				case EAnimNodeCoverage::Lossy: ++Lossy; break;
				case EAnimNodeCoverage::Unsupported: ++Unsupported; break;
				}
			});
			TSharedRef<FJsonObject> AnimCoverage = MakeShared<FJsonObject>();
			AnimCoverage->SetNumberField(TEXT("exact"), Exact);
			AnimCoverage->SetNumberField(TEXT("reflected"), Reflected);
			AnimCoverage->SetNumberField(TEXT("lossy"), Lossy);
			AnimCoverage->SetNumberField(TEXT("unsupported"), Unsupported);
			AnimEntry->SetObjectField(TEXT("coverage"), AnimCoverage);
			ManifestModules.Add(MakeShared<FJsonValueObject>(AnimEntry));

			TArray<FString> RigOutputPaths;
			bool bRigPathInvalid = false;
			for (const FAnimLispImport& Import : AST->RigImports)
			{
				const FString RigOutputPath = FBP2FPMappingRegistry::BlueprintToDSLPath(
					Import.Target.AssetPath, TEXT("Rig"), TEXT(".riglang"));
				const bool bSafeRigPath = AnimBP2FPCommandlets::ValidateMappedOutputPath(
					RigOutputPath, WriteError);
				if (!bSafeRigPath)
				{
					UE_LOG(LogTemp, Error, TEXT("  %s"), *WriteError);
					++FailCount;
					bRigPathInvalid = true;
					FailureCleanup();
					if (bIncludeRigModules) return 1;
				}
				RigOutputPaths.Add(RigOutputPath);
				if (bSafeRigPath)
				{
					BundleRevokeSet.Add(FPaths::ConvertRelativePathToFull(RigOutputPath));
				}
			}
			if (bRigPathInvalid || !AnimBP2FPCommandlets::ValidateUniqueOutputPaths(RigOutputPaths, WriteError))
			{
				UE_LOG(LogTemp, Error, TEXT("  %s"), *WriteError);
				++FailCount;
				FailureCleanup();
				if (bIncludeRigModules) return 1;
				continue;
			}
			for (int32 RigIndex = 0; RigIndex < AST->RigImports.Num(); ++RigIndex)
			{
				const FAnimLispImport& Import = AST->RigImports[RigIndex];
				TSharedRef<FJsonObject> RigEntry = MakeShared<FJsonObject>();
				RigEntry->SetStringField(TEXT("module_identity"), TEXT("rig:") + Import.Target.AssetPath);
				RigEntry->SetStringField(TEXT("source_asset"), Import.Target.AssetPath);
				RigEntry->SetStringField(TEXT("output_path"), RigOutputPaths[RigIndex]);
				RigEntry->SetStringField(TEXT("canonical_hash"), TEXT("unavailable"));
				RigEntry->SetStringField(TEXT("status"), TEXT("failed"));
				RigEntry->SetArrayField(TEXT("dependencies"), {});
				TSharedRef<FJsonObject> RigCoverage = MakeShared<FJsonObject>();
				RigCoverage->SetNumberField(TEXT("exact"), 0);
				RigCoverage->SetNumberField(TEXT("reflected"), 0);
				RigCoverage->SetNumberField(TEXT("lossy"), 0);
				RigCoverage->SetNumberField(TEXT("unsupported"), 0);
				RigEntry->SetObjectField(TEXT("coverage"), RigCoverage);
				const FRigLangExportResult* CachedRigExport = RigExportCache.Find(Import.Target.AssetPath);
				const FRigLangExportResult RigExport = CachedRigExport
					? *CachedRigExport : FRigLangExportResult();
				if (!RigExport.bSuccess || !RigExport.Module.IsValid())
				{
					UE_LOG(LogTemp, Error, TEXT("  Rig pair export failed: %s (%s)"),
						*Import.Target.AssetPath, *WriteError);
					ManifestModules.Add(MakeShared<FJsonValueObject>(RigEntry));
					++FailCount;
					FailureCleanup();
					if (bIncludeRigModules) return 1;
					continue;
				}
				const FRigCoverageTotals Coverage = RigExport.Module->GetCoverageTotals();
				RigEntry->SetStringField(TEXT("canonical_hash"), RigExport.Module->Header.ContentHash);
				RigEntry->SetStringField(TEXT("status"), TEXT("success"));
				TArray<TSharedPtr<FJsonValue>> RigDependencies;
				for (const FRigImportAST& Dependency : RigExport.Module->Imports)
				{
					RigDependencies.Add(MakeShared<FJsonValueString>(Dependency.Import.Target.AssetPath));
				}
				RigEntry->SetArrayField(TEXT("dependencies"), RigDependencies);
				RigCoverage->SetNumberField(TEXT("exact"), Coverage.Exact);
				RigCoverage->SetNumberField(TEXT("reflected"), Coverage.Reflected);
				RigCoverage->SetNumberField(TEXT("lossy"), Coverage.Lossy);
				RigCoverage->SetNumberField(TEXT("unsupported"), Coverage.Unsupported);
				RigEntry->SetObjectField(TEXT("coverage"), RigCoverage);
				ManifestModules.Add(MakeShared<FJsonValueObject>(RigEntry));
				BundleOutputs.Add({
					RigOutputPaths[RigIndex], RigExport.Module->ToCanonicalString()});
			}
		}
		
		// Print DSL to console for review
		UE_LOG(LogTemp, Log, TEXT("--- DSL Output for %s ---"), *AssetName);
		UE_LOG(LogTemp, Log, TEXT("%s"), *DSLOutput);
		UE_LOG(LogTemp, Log, TEXT("--- End DSL Output ---"));
		
		AllOutput += FString::Printf(TEXT("\n;; ======== %s ========\n"), *AssetName);
		AllOutput += DSLOutput;
		AllOutput += TEXT("\n");
	}
	
	// Write combined file
	FString CombinedPath = OutputDir / TEXT("_all_animbp.animlang");
	FString CombinedContent = FString::Printf(
		TEXT(";; AnimLang DSL - All Animation Blueprints\n")
		TEXT(";; Total: %d exported, %d failed\n")
		TEXT(";;\n\n")
		TEXT("%s"),
		SuccessCount, FailCount, *AllOutput
	);
	FFileHelper::SaveStringToFile(CombinedContent, *CombinedPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (bIncludeRigModules)
	{
		TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetStringField(TEXT("schema"), TEXT("animlisp-bundle-v1"));
		Manifest->SetArrayField(TEXT("modules"), ManifestModules);
		FString ManifestJson;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ManifestJson);
		FJsonSerializer::Serialize(Manifest, Writer);
		FString ManifestError;
		bool bCommitted = false;
		if (FailCount == 0
			&& AnimBP2FPCommandlets::ValidateBundleManifestJson(ManifestJson, ManifestError))
		{
			bCommitted = AnimBP2FPCommandlets::CommitBundleAtomically(
				BundleOutputs, ManifestPath, ManifestJson, BundleRevokeSet, ManifestError);
			if (bCommitted)
			{
				bCommitted = AnimBP2FPCommandlets::ValidateBundleOutputHashes(
					ManifestJson, ManifestError);
			}
		}
		if (!bCommitted)
		{
			FailureCleanup();
			UE_LOG(LogTemp, Error, TEXT("%s"), *ManifestError);
			if (FailCount == 0) ++FailCount;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("=== Export Complete: %d/%d succeeded. Output: %s ==="),
		SuccessCount, AnimBPAssets.Num(), *OutputDir);

	return (FailCount > 0) ? 1 : 0;
}
