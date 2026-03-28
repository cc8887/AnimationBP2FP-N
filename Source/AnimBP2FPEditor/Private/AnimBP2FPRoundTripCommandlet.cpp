// AnimBP2FPRoundTripCommandlet.cpp - Round-trip validation implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPRoundTripCommandlet.h"
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
