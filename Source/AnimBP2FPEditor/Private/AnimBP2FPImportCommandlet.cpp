// AnimBP2FPImportCommandlet.cpp - DSL Import commandlet implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPImportCommandlet.h"
#include "AnimBPImporter.h"
#include "AnimBPExporter.h"
#include "AnimLangParser.h"
#include "AnimLangRoundTrip.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimBPImportCmd, Log, All);

UAnimBP2FPImportCommandlet::UAnimBP2FPImportCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

UAnimBlueprint* UAnimBP2FPImportCommandlet::ImportFile(const FString& FilePath, const FString& OutputPackagePath, FString& OutError)
{
	// Read the file
	FString DSLCode;
	if (!FFileHelper::LoadFileToString(DSLCode, *FilePath))
	{
		OutError = FString::Printf(TEXT("Could not read file: %s"), *FilePath);
		return nullptr;
	}
	
	UE_LOG(LogAnimBPImportCmd, Log, TEXT("Importing: %s (%d chars)"), *FilePath, DSLCode.Len());
	
	// Import
	UAnimBlueprint* Blueprint = FAnimBPImporter::Import(DSLCode, OutputPackagePath, &OutError);
	
	return Blueprint;
}

bool UAnimBP2FPImportCommandlet::RunImportRoundTrip(const FString& FilePath, const FString& OutputPackagePath, FString& OutReport)
{
	// Step 1: Read the original DSL
	FString OriginalDSL;
	if (!FFileHelper::LoadFileToString(OriginalDSL, *FilePath))
	{
		OutReport = FString::Printf(TEXT("FAIL: Could not read %s"), *FilePath);
		return false;
	}
	
	FString FileName = FPaths::GetBaseFilename(FilePath);
	
	// Step 2: Import DSL → Blueprint
	FString ImportError;
	UAnimBlueprint* Blueprint = FAnimBPImporter::Import(OriginalDSL, OutputPackagePath, &ImportError);
	if (!Blueprint)
	{
		OutReport = FString::Printf(TEXT("FAIL [%s]: Import failed — %s"), *FileName, *ImportError);
		return false;
	}
	
	// Step 3: Export Blueprint → DSL
	FString ReExportedDSL = FAnimBPExporter::Export(Blueprint);
	if (ReExportedDSL.IsEmpty())
	{
		OutReport = FString::Printf(TEXT("FAIL [%s]: Re-export produced empty result"), *FileName);
		return false;
	}
	
	// Step 4: Compare — use the round-trip comparison infrastructure
	// We need to strip the header comments for comparison
	auto StripComments = [](const FString& Input) -> FString
	{
		FString Result;
		TArray<FString> Lines;
		Input.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString Trimmed = Line.TrimStartAndEnd();
			if (!Trimmed.StartsWith(TEXT(";;")))
			{
				Result += Line + TEXT("\n");
			}
		}
		return Result.TrimStartAndEnd();
	};
	
	FString OrigClean = StripComments(OriginalDSL);
	FString ReExpClean = StripComments(ReExportedDSL);
	
	// Normalize both for comparison (ignore trailing whitespace, empty lines)
	TArray<FString> OrigLines, ReExpLines;
	OrigClean.ParseIntoArrayLines(OrigLines);
	ReExpClean.ParseIntoArrayLines(ReExpLines);
	
	// Remove empty lines
	OrigLines.RemoveAll([](const FString& L) { return L.TrimStartAndEnd().IsEmpty(); });
	ReExpLines.RemoveAll([](const FString& L) { return L.TrimStartAndEnd().IsEmpty(); });
	
	int32 MatchCount = 0;
	int32 DiffCount = 0;
	FString DiffDetails;
	
	int32 MaxLines = FMath::Max(OrigLines.Num(), ReExpLines.Num());
	for (int32 i = 0; i < MaxLines; i++)
	{
		FString OLine = (i < OrigLines.Num()) ? OrigLines[i].TrimEnd() : TEXT("<missing>");
		FString RLine = (i < ReExpLines.Num()) ? ReExpLines[i].TrimEnd() : TEXT("<missing>");
		
		if (OLine == RLine)
		{
			MatchCount++;
		}
		else
		{
			DiffCount++;
			if (DiffCount <= 10)  // Limit diff output
			{
				DiffDetails += FString::Printf(TEXT("  Line %d:\n    Original:   %s\n    Re-export:  %s\n"), i + 1, *OLine, *RLine);
			}
		}
	}
	
	float Similarity = (MaxLines > 0) ? (float)MatchCount / (float)MaxLines * 100.0f : 100.0f;
	bool bPass = (DiffCount == 0);
	
	OutReport = FString::Printf(TEXT("%s [%s]: %s (%.1f%% similar, %d diffs)"),
		bPass ? TEXT("PASS") : TEXT("FAIL"),
		*FileName,
		bPass ? TEXT("Import round-trip perfect") : TEXT("Differences found"),
		Similarity, DiffCount);
	
	if (!DiffDetails.IsEmpty())
	{
		OutReport += TEXT("\n") + DiffDetails;
	}
	
	return bPass;
}

// ========== Simulated Edit ==========

FString UAnimBP2FPImportCommandlet::ApplySimulatedEdit(const FString& OriginalDSL)
{
	// Apply a simple but verifiable property change:
	// - If DSL contains ":loop true", change to ":loop false"
	// - If DSL contains ":loop false", change to ":loop true"
	// - If neither, add " :test-marker 42" before the first closing paren of anim-graph
	
	FString Modified = OriginalDSL;
	
	// Strategy 1: Toggle a :loop value
	if (Modified.Contains(TEXT(":loop true")))
	{
		// Only replace the FIRST occurrence to make a minimal change
		Modified.ReplaceInline(TEXT(":loop true"), TEXT(":loop false"), ESearchCase::CaseSensitive);
		return Modified;
	}
	
	if (Modified.Contains(TEXT(":loop false")))
	{
		Modified.ReplaceInline(TEXT(":loop false"), TEXT(":loop true"), ESearchCase::CaseSensitive);
		return Modified;
	}
	
	// Strategy 2: If there's a float property, tweak it slightly
	// Find ":alpha 1.000000" and change to ":alpha 0.500000"
	if (Modified.Contains(TEXT(":alpha 1.000000")))
	{
		Modified.ReplaceInline(TEXT(":alpha 1.000000"), TEXT(":alpha 0.500000"), ESearchCase::CaseSensitive);
		return Modified;
	}
	
	// Strategy 3: If nothing obvious to change, just return the same DSL
	// This will test the "no changes" path
	return Modified;
}

// ========== Update Test ==========

bool UAnimBP2FPImportCommandlet::RunUpdateTest(const FString& FilePath, const FString& OutputPackagePath, FString& OutReport)
{
	FString FileName = FPaths::GetBaseFilename(FilePath);
	
	// Step 1: Read the original DSL
	FString OriginalDSL;
	if (!FFileHelper::LoadFileToString(OriginalDSL, *FilePath))
	{
		OutReport = FString::Printf(TEXT("FAIL [%s]: Could not read %s"), *FileName, *FilePath);
		return false;
	}
	
	// Step 2: Import original DSL → Blueprint
	FString ImportError;
	FString ImportPath = OutputPackagePath / (FileName + TEXT("_Update"));
	UAnimBlueprint* Blueprint = FAnimBPImporter::Import(OriginalDSL, ImportPath, &ImportError);
	if (!Blueprint)
	{
		OutReport = FString::Printf(TEXT("FAIL [%s]: Initial import failed — %s"), *FileName, *ImportError);
		return false;
	}
	
	// Step 3: Export to get the "current state" DSL (this is the baseline)
	FString BaselineDSL = FAnimBPExporter::Export(Blueprint);
	if (BaselineDSL.IsEmpty())
	{
		OutReport = FString::Printf(TEXT("FAIL [%s]: Baseline export failed"), *FileName);
		return false;
	}
	
	// Step 4: Create a modified DSL (simulated user edit)
	FString ModifiedDSL = ApplySimulatedEdit(BaselineDSL);
	bool bSameDSL = (ModifiedDSL == BaselineDSL);
	
	// Step 5: Run UpdateBlueprint
	FAnimBPImporter::FUpdateResult UpdateResult = FAnimBPImporter::UpdateBlueprintDetailed(Blueprint, ModifiedDSL);
	
	if (!UpdateResult.bSuccess)
	{
		OutReport = FString::Printf(TEXT("FAIL [%s]: UpdateBlueprint failed\n  %s"),
			*FileName, *UpdateResult.ToString());
		return false;
	}
	
	// Step 6: Export the updated blueprint
	FString UpdatedDSL = FAnimBPExporter::Export(Blueprint);
	if (UpdatedDSL.IsEmpty())
	{
		OutReport = FString::Printf(TEXT("FAIL [%s]: Post-update export failed"), *FileName);
		return false;
	}
	
	// Step 7: Compare updated DSL with modified DSL
	// Strip comments for comparison
	auto StripComments = [](const FString& Input) -> FString
	{
		FString Result;
		TArray<FString> Lines;
		Input.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString Trimmed = Line.TrimStartAndEnd();
			if (!Trimmed.StartsWith(TEXT(";;")))
			{
				Result += Line + TEXT("\n");
			}
		}
		return Result.TrimStartAndEnd();
	};
	
	FString ModClean = StripComments(ModifiedDSL);
	FString UpdClean = StripComments(UpdatedDSL);
	
	TArray<FString> ModLines, UpdLines;
	ModClean.ParseIntoArrayLines(ModLines);
	UpdClean.ParseIntoArrayLines(UpdLines);
	ModLines.RemoveAll([](const FString& L) { return L.TrimStartAndEnd().IsEmpty(); });
	UpdLines.RemoveAll([](const FString& L) { return L.TrimStartAndEnd().IsEmpty(); });
	
	int32 MatchCount = 0;
	int32 DiffCount = 0;
	FString DiffDetails;
	
	int32 MaxLines = FMath::Max(ModLines.Num(), UpdLines.Num());
	for (int32 i = 0; i < MaxLines; i++)
	{
		FString MLine = (i < ModLines.Num()) ? ModLines[i].TrimEnd() : TEXT("<missing>");
		FString ULine = (i < UpdLines.Num()) ? UpdLines[i].TrimEnd() : TEXT("<missing>");
		
		if (MLine == ULine)
		{
			MatchCount++;
		}
		else
		{
			DiffCount++;
			if (DiffCount <= 10)
			{
				DiffDetails += FString::Printf(TEXT("  Line %d:\n    Expected: %s\n    Got:      %s\n"), i + 1, *MLine, *ULine);
			}
		}
	}
	
	float Similarity = (MaxLines > 0) ? (float)MatchCount / (float)MaxLines * 100.0f : 100.0f;
	bool bPass = (DiffCount == 0);
	
	// Build report
	OutReport = FString::Printf(TEXT("%s [%s]: Update %s (%.1f%% match, %d diffs, %s, %d changes)"),
		bPass ? TEXT("PASS") : TEXT("FAIL"),
		*FileName,
		bPass ? TEXT("verified") : TEXT("differences found"),
		Similarity, DiffCount,
		UpdateResult.bUsedIncrementalPatch ? TEXT("incremental") : TEXT("rebuild"),
		UpdateResult.NumChanges);
	
	if (bSameDSL)
	{
		OutReport += TEXT(" [no-op edit — DSL unchanged]");
	}
	
	if (!DiffDetails.IsEmpty())
	{
		OutReport += TEXT("\n") + DiffDetails;
	}
	
	// Also include update result warnings
	for (const FString& W : UpdateResult.Warnings)
	{
		OutReport += TEXT("\n  Warning: ") + W;
	}
	
	return bPass;
}

// ========== Main ==========

int32 UAnimBP2FPImportCommandlet::Main(const FString& Params)
{
	UE_LOG(LogAnimBPImportCmd, Log, TEXT("=== AnimBP2FP Import Commandlet ==="));
	
	// Parse arguments
	FString FileArg, OutDirArg;
	bool bTestMode = false;
	bool bUpdateMode = false;
	
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamMap;
	ParseCommandLine(*Params, Tokens, Switches, ParamMap);
	
	FileArg = ParamMap.FindRef(TEXT("file"));
	OutDirArg = ParamMap.FindRef(TEXT("outdir"));
	bTestMode = Switches.Contains(TEXT("test"));
	bUpdateMode = Switches.Contains(TEXT("update"));
	const bool bLegacyMode = Switches.Contains(TEXT("legacy"));
	FString BundleDir = ParamMap.FindRef(TEXT("bundle"));
	if (BundleDir.IsEmpty()) BundleDir = ParamMap.FindRef(TEXT("workspace"));
	
	if (OutDirArg.IsEmpty())
	{
		OutDirArg = TEXT("/Game/AnimBP2FP/Imported");
	}

	if (!BundleDir.IsEmpty())
	{
		TArray<FString> BundleFiles;
		TArray<FString> RigFiles;
		IFileManager::Get().FindFilesRecursive(BundleFiles, *BundleDir, TEXT("*.animlang"), true, false);
		IFileManager::Get().FindFilesRecursive(RigFiles, *BundleDir, TEXT("*.riglang"), true, false);
		BundleFiles.Append(RigFiles);
		BundleFiles.Sort();
		TArray<FAnimLispBundleSource> Sources;
		for (const FString& BundleFile : BundleFiles)
		{
			FString Source;
			if (!FFileHelper::LoadFileToString(Source, *BundleFile))
			{
				UE_LOG(LogAnimBPImportCmd, Error, TEXT("Could not read bundle source: %s"), *BundleFile);
				return 1;
			}
			Sources.Add({BundleFile, MoveTemp(Source)});
		}
		FAnimLispBundleImportOptions BundleOptions;
		BundleOptions.Mode = bLegacyMode
			? EAnimLispBundleImportMode::Legacy : EAnimLispBundleImportMode::Strict;
		BundleOptions.TargetRoot = OutDirArg;
		BundleOptions.bCommitPersistent = true;
		const FAnimLispBundleImportResult BundleResult = FAnimBPImporter::ImportBundle(Sources, BundleOptions);
		UE_LOG(LogAnimBPImportCmd, Log, TEXT("%s"), *BundleResult.Diagnostics.ToReport());
		if (!BundleResult.bSuccess)
		{
			UE_LOG(LogAnimBPImportCmd, Error, TEXT("Bundle import failed %s persistent target mutation"),
				BundleResult.bMutationStarted ? TEXT("after") : TEXT("before"));
			return 1;
		}
		UE_LOG(LogAnimBPImportCmd, Log, TEXT("Bundle import committed: %d assets, mode=%s"),
			BundleResult.StagedAssets.Num(), bLegacyMode ? TEXT("Legacy") : TEXT("Strict"));
		return 0;
	}

	if (!bLegacyMode)
	{
		UE_LOG(LogAnimBPImportCmd, Error,
			TEXT("Single-file Anim import is legacy behavior; pass -Legacy explicitly or use -Bundle=<workspace>"));
		return 1;
	}
	
	// Collect files to process
	TArray<FString> FilesToProcess;
	
	if (!FileArg.IsEmpty())
	{
		FilesToProcess.Add(FileArg);
	}
	else
	{
		// Default: all .animlang files from export directory
		FString ExportDir = FPaths::ProjectDir() / TEXT("AnimLang") / TEXT("Exported");
		IFileManager::Get().FindFiles(FilesToProcess, *(ExportDir / TEXT("*.animlang")), true, false);
		
		// Make paths absolute and filter out aggregate files
		TArray<FString> AbsPaths;
		for (const FString& F : FilesToProcess)
		{
			if (!F.StartsWith(TEXT("_")))  // Skip _all_animbp.animlang etc.
			{
				AbsPaths.Add(ExportDir / F);
			}
		}
		FilesToProcess = AbsPaths;
	}
	
	if (FilesToProcess.Num() == 0)
	{
		UE_LOG(LogAnimBPImportCmd, Error, TEXT("No .animlang files found to import"));
		return 1;
	}
	
	FString ModeStr = bUpdateMode ? TEXT("update") : (bTestMode ? TEXT("test") : TEXT("import"));
	UE_LOG(LogAnimBPImportCmd, Log, TEXT("Processing %d files (output: %s, mode: %s)"),
		FilesToProcess.Num(), *OutDirArg, *ModeStr);
	
	int32 SuccessCount = 0;
	int32 FailCount = 0;
	FString FullReport;
	FullReport += FString::Printf(TEXT("=== AnimBP2FP %s Report ===\n\n"),
		bUpdateMode ? TEXT("Update") : (bTestMode ? TEXT("Import") : TEXT("Import")));
	
	for (const FString& FilePath : FilesToProcess)
	{
		FString FileName = FPaths::GetBaseFilename(FilePath);
		
		if (bUpdateMode)
		{
			FString Report;
			bool bPass = RunUpdateTest(FilePath, OutDirArg, Report);
			FullReport += Report + TEXT("\n");
			UE_LOG(LogAnimBPImportCmd, Log, TEXT("Update test [%s]: %s"), *FileName, bPass ? TEXT("PASS") : TEXT("FAIL"));
			if (bPass) SuccessCount++; else FailCount++;
		}
		else if (bTestMode)
		{
			FString Report;
			bool bPass = RunImportRoundTrip(FilePath, OutDirArg, Report);
			FullReport += Report + TEXT("\n");
			UE_LOG(LogAnimBPImportCmd, Log, TEXT("Import round-trip [%s]: %s"), *FileName, bPass ? TEXT("PASS") : TEXT("FAIL"));
			if (bPass) SuccessCount++; else FailCount++;
		}
		else
		{
			FString Error;
			UAnimBlueprint* BP = ImportFile(FilePath, OutDirArg, Error);
			if (BP)
			{
				FullReport += FString::Printf(TEXT("OK [%s]: Created %s\n"), *FileName, *BP->GetPathName());
				SuccessCount++;
			}
			else
			{
				FullReport += FString::Printf(TEXT("FAIL [%s]: %s\n"), *FileName, *Error);
				FailCount++;
			}
		}
	}
	
	FullReport += FString::Printf(TEXT("\nTotal: %d files, %d succeeded, %d failed\n"),
		FilesToProcess.Num(), SuccessCount, FailCount);
	
	// Print summary
	UE_LOG(LogAnimBPImportCmd, Log, TEXT("\n%s"), *FullReport);
	
	// Save report
	FString ReportName = bUpdateMode ? TEXT("update_report.txt") : TEXT("import_report.txt");
	FString ReportPath = FPaths::ProjectDir() / TEXT("AnimLang") / TEXT("Exported") / ReportName;
	FFileHelper::SaveStringToFile(FullReport, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogAnimBPImportCmd, Log, TEXT("Report saved to: %s"), *ReportPath);
	
	return (FailCount > 0) ? 1 : 0;
}
