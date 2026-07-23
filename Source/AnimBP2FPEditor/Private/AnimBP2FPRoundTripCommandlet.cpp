// AnimBP2FPRoundTripCommandlet.cpp - Round-trip validation implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPRoundTripCommandlet.h"
#include "AnimBPImporter.h"
#include "AnimLangRoundTrip.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

UAnimBP2FPRoundTripCommandlet::UAnimBP2FPRoundTripCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UAnimBP2FPRoundTripCommandlet::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("=== AnimBP2FP Round-Trip Validation ==="));
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamMap;
	ParseCommandLine(*Params, Tokens, Switches, ParamMap);
	const bool bLegacyMode = Switches.Contains(TEXT("legacy"));
	FString BundleDir = ParamMap.FindRef(TEXT("bundle"));
	if (BundleDir.IsEmpty()) BundleDir = ParamMap.FindRef(TEXT("workspace"));
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
				UE_LOG(LogTemp, Error, TEXT("Could not read bundle source: %s"), *BundleFile);
				return 1;
			}
			Sources.Add({BundleFile, MoveTemp(Source)});
		}
		FAnimLispBundleImportOptions Options;
		Options.Mode = bLegacyMode
			? EAnimLispBundleImportMode::Legacy : EAnimLispBundleImportMode::Strict;
		Options.TargetRoot = ParamMap.FindRef(TEXT("outdir"));
		if (Options.TargetRoot.IsEmpty()) Options.TargetRoot = TEXT("/Engine/Transient/AnimLispCommandletRoundTrip");
		const FAnimLispBundleImportResult Result = FAnimBPImporter::ImportBundle(Sources, Options);
		UE_LOG(LogTemp, Log, TEXT("%s"), *Result.Diagnostics.ToReport());
		return Result.bSuccess ? 0 : 1;
	}
	if (!bLegacyMode)
	{
		UE_LOG(LogTemp, Error,
			TEXT("Legacy Anim-only round-trip requires -Legacy; use -Bundle=<workspace> for strict Anim/Rig validation"));
		return 1;
	}
	
	FString ExportDir = FPaths::ProjectDir() / TEXT("AnimLang") / TEXT("Exported");
	
	if (!IFileManager::Get().DirectoryExists(*ExportDir))
	{
		UE_LOG(LogTemp, Error, TEXT("Export directory not found: %s"), *ExportDir);
		UE_LOG(LogTemp, Error, TEXT("Run the export first: -run=AnimBP2FPExport"));
		return 1;
	}
	
	// Run tests
	TArray<FRoundTripResult> Results = FAnimLangRoundTrip::TestDirectory(ExportDir);
	
	if (Results.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("No .animlang files found in: %s"), *ExportDir);
		return 1;
	}
	
	// Generate report
	FString Report = FAnimLangRoundTrip::GenerateReport(Results);
	
	// Print to console
	UE_LOG(LogTemp, Log, TEXT("\n%s"), *Report);
	
	// Save to file
	FString ReportPath = ExportDir / TEXT("round_trip_report.txt");
	FFileHelper::SaveStringToFile(Report, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogTemp, Log, TEXT("Report saved to: %s"), *ReportPath);
	
	// Return exit code based on results
	int32 FailCount = 0;
	for (const FRoundTripResult& R : Results)
	{
		if (!R.bSuccess) FailCount++;
	}
	
	return (FailCount > 0) ? 1 : 0;
}
