// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// AnimBP2FPBlueprintLispCommandlet.cpp

#include "AnimBP2FPBlueprintLispCommandlet.h"
#include "AnimBPExporter.h"           // FAnimBPExporter::ExportEventGraph
#include "BlueprintLispAST.h"         // FLispParser, BlueprintLisp::*
#include "BlueprintLispConverter.h"   // FBlueprintLispConverter::Validate

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintLispTest, Log, All);

// ============================================================================
// Constructor
// ============================================================================

UAnimBP2FPBlueprintLispCommandlet::UAnimBP2FPBlueprintLispCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

// ============================================================================
// Main entry point
// ============================================================================

int32 UAnimBP2FPBlueprintLispCommandlet::Main(const FString& Params)
{
	UE_LOG(LogBlueprintLispTest, Log, TEXT(""));
	UE_LOG(LogBlueprintLispTest, Log, TEXT("=========================================================="));
	UE_LOG(LogBlueprintLispTest, Log, TEXT("  AnimBP2FP + BlueprintLisp: EventGraph Export Test"));
	UE_LOG(LogBlueprintLispTest, Log, TEXT("=========================================================="));

	// ---- Parse command-line ----
	TArray<FString> Tokens, Switches;
	TMap<FString, FString> SwitchParams;
	ParseCommandLine(*Params, Tokens, Switches, SwitchParams);

	FString TargetBP  = SwitchParams.FindRef(TEXT("bp"));
	FString GraphName = SwitchParams.Contains(TEXT("graph")) ? SwitchParams[TEXT("graph")] : TEXT("EventGraph");
	FString OutputDir = SwitchParams.Contains(TEXT("outdir")) ? SwitchParams[TEXT("outdir")]
	                    : (FPaths::ProjectDir() / TEXT("AnimLang") / TEXT("EventGraph"));
	bool bRoundTrip   = Switches.Contains(TEXT("roundtrip"));
	bool bNoWrite     = Switches.Contains(TEXT("nowrite"));

	UE_LOG(LogBlueprintLispTest, Log, TEXT("  Graph:      %s"), *GraphName);
	UE_LOG(LogBlueprintLispTest, Log, TEXT("  RoundTrip:  %s"), bRoundTrip ? TEXT("YES") : TEXT("NO"));
	UE_LOG(LogBlueprintLispTest, Log, TEXT("  WriteFiles: %s"), bNoWrite   ? TEXT("NO")  : TEXT("YES"));
	if (!TargetBP.IsEmpty())
		UE_LOG(LogBlueprintLispTest, Log, TEXT("  Target BP:  %s"), *TargetBP);
	UE_LOG(LogBlueprintLispTest, Log, TEXT("=========================================================="));

	// ---- Ensure output dir ----
	if (!bNoWrite && !IFileManager::Get().DirectoryExists(*OutputDir))
		IFileManager::Get().MakeDirectory(*OutputDir, true);

	// ---- Collect AnimBlueprints ----
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	AR.SearchAllAssets(true);

	TArray<FAssetData> Assets;
	if (!TargetBP.IsEmpty())
	{
		// Single BP by path
		FAssetData AD = AR.GetAssetByObjectPath(FSoftObjectPath(TargetBP));
		if (AD.IsValid()) Assets.Add(AD);
		else
		{
			UE_LOG(LogBlueprintLispTest, Error, TEXT("Blueprint not found: %s"), *TargetBP);
			return 1;
		}
	}
	else
	{
		AR.GetAssetsByClass(UAnimBlueprint::StaticClass()->GetClassPathName(), Assets);
	}

	if (Assets.IsEmpty())
	{
		UE_LOG(LogBlueprintLispTest, Warning, TEXT("No AnimBlueprints found."));
		return 0;
	}

	UE_LOG(LogBlueprintLispTest, Log, TEXT("Found %d AnimBlueprint(s)"), Assets.Num());

	// ---- Run tests ----
	TArray<FTestResult> Results;
	for (const FAssetData& AD : Assets)
	{
		UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(AD.GetAsset());
		if (!AnimBP)
		{
			FTestResult R;
			R.AssetName = AD.AssetName.ToString();
			R.AssetPath = AD.PackageName.ToString();
			R.Error     = TEXT("Failed to load asset");
			Results.Add(R);
			continue;
		}
		Results.Add(ExportOne(AnimBP, GraphName, bRoundTrip, !bNoWrite, OutputDir));
	}

	// ---- Report ----
	WriteReport(Results, OutputDir);

	// Return 0 if all exports succeeded (ignoring blueprints with no EventGraph)
	int32 Failures = 0;
	for (const FTestResult& R : Results)
		if (!R.bExportOk && !R.Error.Contains(TEXT("not found"))) Failures++;

	return Failures > 0 ? 1 : 0;
}

// ============================================================================
// ExportOne
// ============================================================================

UAnimBP2FPBlueprintLispCommandlet::FTestResult
UAnimBP2FPBlueprintLispCommandlet::ExportOne(
	UAnimBlueprint* AnimBP,
	const FString&  GraphName,
	bool            bRoundTrip,
	bool            bWriteFile,
	const FString&  OutputDir)
{
	FTestResult R;
	R.AssetName = AnimBP->GetName();
	R.AssetPath = AnimBP->GetPathName();
	R.GraphName = GraphName;

	UE_LOG(LogBlueprintLispTest, Log, TEXT(""));
	UE_LOG(LogBlueprintLispTest, Log, TEXT("--- %s [%s] ---"), *R.AssetName, *GraphName);

	// Export EventGraph via BlueprintLisp
	FAnimBPExporter::FEventGraphExportOptions ExportOpts;
	ExportOpts.GraphName        = GraphName;
	ExportOpts.bPrettyPrint     = true;
	ExportOpts.bStableIds       = true;

	FString LispCode;
	FString ExportError;
	R.bExportOk = FAnimBPExporter::ExportEventGraph(AnimBP, ExportOpts, LispCode, ExportError);

	if (!R.bExportOk)
	{
		R.Error = ExportError;
		UE_LOG(LogBlueprintLispTest, Warning, TEXT("  EXPORT FAILED: %s"), *ExportError);
		return R;
	}

	TArray<FString> LispLines;
	LispCode.ParseIntoArrayLines(LispLines, false);
	R.LispLineCount = LispLines.Num();

	// Count events
	FLispParseResult PR = FLispParser::Parse(LispCode);
	if (PR.bSuccess)
	{
		R.EventCount = PR.Nodes.Num();
		for (const auto& N : PR.Nodes)
		{
			FString Form = N->GetFormName();
			UE_LOG(LogBlueprintLispTest, Log, TEXT("  event: %s"),
				N->Num() > 1 ? *N->Get(1)->ToString(false, 0) : TEXT("?"));
		}
	}

	UE_LOG(LogBlueprintLispTest, Log, TEXT("  Export OK: %d event(s), %d lines"), R.EventCount, R.LispLineCount);
	UE_LOG(LogBlueprintLispTest, Verbose, TEXT("  DSL preview:\n%s"), *LispCode.Left(500));

	// Round-trip validation
	if (bRoundTrip)
	{
		R.bRoundTripOk = ValidateRoundTrip(LispCode, R.RoundTripError);
		if (R.bRoundTripOk)
		{
			UE_LOG(LogBlueprintLispTest, Log, TEXT("  RoundTrip OK"));
		}
		else
		{
			UE_LOG(LogBlueprintLispTest, Warning, TEXT("  RoundTrip FAIL: %s"), *R.RoundTripError);
		}
	}

	// Write file
	if (bWriteFile)
	{
		FString SafeName = R.AssetName.Replace(TEXT("/"), TEXT("_")).Replace(TEXT("."), TEXT("_"));
		FString FilePath = OutputDir / FString::Printf(TEXT("%s.bplisp"), *SafeName);
		FString Content  = FString::Printf(
			TEXT(";; BlueprintLisp DSL - EventGraph Export\n")
			TEXT(";; Blueprint: %s\n")
			TEXT(";; Graph:     %s\n")
			TEXT(";; Events:    %d\n")
			TEXT(";; Generated by AnimBP2FP BlueprintLisp plugin\n")
			TEXT("\n%s"),
			*R.AssetPath, *GraphName, R.EventCount, *LispCode);

		if (FFileHelper::SaveStringToFile(Content, *FilePath))
		{
			UE_LOG(LogBlueprintLispTest, Log, TEXT("  Saved: %s"), *FilePath);
		}
		else
		{
			UE_LOG(LogBlueprintLispTest, Warning, TEXT("  Failed to save: %s"), *FilePath);
		}
	}

	return R;
}

// ============================================================================
// ValidateRoundTrip
// ============================================================================

bool UAnimBP2FPBlueprintLispCommandlet::ValidateRoundTrip(const FString& LispCode, FString& OutError)
{
	// Pass 1: Parse original
	FLispParseResult R1 = FLispParser::Parse(LispCode);
	if (!R1.bSuccess)
	{
		OutError = FString::Printf(TEXT("Pass1 parse failed: %s"), *R1.Error);
		return false;
	}

	// Pass 1: Serialize (minified for comparison)
	FString S1;
	for (int32 i = 0; i < R1.Nodes.Num(); i++)
	{
		if (i > 0) S1 += TEXT(" ");
		S1 += R1.Nodes[i]->ToString(false, 0);
	}

	// Pass 2: Re-parse from minified
	FLispParseResult R2 = FLispParser::Parse(S1);
	if (!R2.bSuccess)
	{
		OutError = FString::Printf(TEXT("Pass2 parse failed: %s"), *R2.Error);
		return false;
	}

	// Pass 2: Serialize again
	FString S2;
	for (int32 i = 0; i < R2.Nodes.Num(); i++)
	{
		if (i > 0) S2 += TEXT(" ");
		S2 += R2.Nodes[i]->ToString(false, 0);
	}

	if (S1 != S2)
	{
		// Find first diff
		int32 DiffPos = 0;
		while (DiffPos < S1.Len() && DiffPos < S2.Len() && S1[DiffPos] == S2[DiffPos]) DiffPos++;
		OutError = FString::Printf(TEXT("S1 != S2 at pos %d; S1[%d..%d]='%s' S2[%d..%d]='%s'"),
			DiffPos,
			FMath::Max(0, DiffPos - 10), FMath::Min(DiffPos + 20, S1.Len()),
			*S1.Mid(FMath::Max(0, DiffPos - 10), 30),
			FMath::Max(0, DiffPos - 10), FMath::Min(DiffPos + 20, S2.Len()),
			*S2.Mid(FMath::Max(0, DiffPos - 10), 30));
		return false;
	}
	return true;
}

// ============================================================================
// WriteReport
// ============================================================================

void UAnimBP2FPBlueprintLispCommandlet::WriteReport(
	const TArray<FTestResult>& Results,
	const FString&             OutputDir)
{
	int32 TotalExport    = 0;
	int32 OkExport       = 0;
	int32 NoGraph        = 0;
	int32 FailExport     = 0;
	int32 TotalRoundTrip = 0;
	int32 OkRoundTrip    = 0;
	int32 TotalEvents    = 0;

	FString Report;
	Report += TEXT("==========================================================\n");
	Report += TEXT("  AnimBP2FP + BlueprintLisp: EventGraph Export Report\n");
	Report += TEXT("==========================================================\n\n");

	for (const FTestResult& R : Results)
	{
		TotalExport++;
		Report += FString::Printf(TEXT("BP: %s\n"), *R.AssetName);
		Report += FString::Printf(TEXT("  Path:    %s\n"), *R.AssetPath);
		Report += FString::Printf(TEXT("  Graph:   %s\n"), *R.GraphName);

		if (!R.bExportOk)
		{
			if (R.Error.Contains(TEXT("not found")) || R.Error.Contains(TEXT("No events found")))
			{
				NoGraph++;
				Report += FString::Printf(TEXT("  Export:  SKIP (%s)\n"), *R.Error);
			}
			else
			{
				FailExport++;
				Report += FString::Printf(TEXT("  Export:  FAIL: %s\n"), *R.Error);
			}
		}
		else
		{
			OkExport++;
			TotalEvents += R.EventCount;
			Report += FString::Printf(TEXT("  Export:  OK  (%d events, %d lines)\n"), R.EventCount, R.LispLineCount);
			if (!R.RoundTripError.IsEmpty())
			{
				TotalRoundTrip++;
				if (R.bRoundTripOk) { OkRoundTrip++; Report += TEXT("  RndTrip: OK\n"); }
				else                 Report += FString::Printf(TEXT("  RndTrip: FAIL: %s\n"), *R.RoundTripError);
			}
		}
		Report += TEXT("\n");
	}

	// Summary
	Report += TEXT("==========================================================\n");
	Report += FString::Printf(TEXT("  Total  : %d\n"), TotalExport);
	Report += FString::Printf(TEXT("  OK     : %d\n"), OkExport);
	Report += FString::Printf(TEXT("  NoGraph: %d (skipped — no EventGraph events)\n"), NoGraph);
	Report += FString::Printf(TEXT("  FAIL   : %d\n"), FailExport);
	Report += FString::Printf(TEXT("  Events : %d total exported\n"), TotalEvents);
	if (TotalRoundTrip > 0)
		Report += FString::Printf(TEXT("  RndTrip: %d/%d PASS\n"), OkRoundTrip, TotalRoundTrip);
	Report += TEXT("==========================================================\n");

	// Log summary
	UE_LOG(LogBlueprintLispTest, Log, TEXT(""));
	UE_LOG(LogBlueprintLispTest, Log, TEXT("=========================================================="));
	UE_LOG(LogBlueprintLispTest, Log, TEXT("  SUMMARY: %d OK / %d skip / %d FAIL  (total events: %d)"),
		OkExport, NoGraph, FailExport, TotalEvents);
	if (TotalRoundTrip > 0)
		UE_LOG(LogBlueprintLispTest, Log, TEXT("  RoundTrip: %d / %d PASS"), OkRoundTrip, TotalRoundTrip);
	UE_LOG(LogBlueprintLispTest, Log, TEXT("=========================================================="));

	// Write report file
	FString ReportPath = OutputDir / TEXT("bplisp_export_report.txt");
	FFileHelper::SaveStringToFile(Report, *ReportPath);
	UE_LOG(LogBlueprintLispTest, Log, TEXT("  Report: %s"), *ReportPath);
}
