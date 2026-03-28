// AnimLangRoundTrip.cpp - Round-trip Validation Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLangRoundTrip.h"
#include "AnimLangParser.h"
#include "AnimLangTokenizer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

// ========== TestString ==========

FRoundTripResult FAnimLangRoundTrip::TestString(const FString& DSLCode, const FString& AssetName)
{
	FRoundTripResult Result;
	Result.AssetName = AssetName;
	
	// Step 1: Normalize the original (strip file header comments, keep the anim-blueprint form)
	FString OriginalBody;
	{
		// Find the first '(' which starts the actual S-expression
		int32 FirstParen = DSLCode.Find(TEXT("("));
		if (FirstParen != INDEX_NONE)
		{
			OriginalBody = DSLCode.Mid(FirstParen);
		}
		else
		{
			OriginalBody = DSLCode;
		}
	}
	
	Result.OriginalText = NormalizeForComparison(OriginalBody);
	
	// Step 2: Parse
	TArray<FAnimLangParseError> ParseErrors;
	TSharedPtr<FAnimGraphAST> AST = FAnimLangParser::Parse(DSLCode, ParseErrors);
	
	for (const FAnimLangParseError& Err : ParseErrors)
	{
		Result.ParseErrors.Add(Err.ToString());
	}
	
	if (!AST.IsValid())
	{
		Result.bSuccess = false;
		Result.ParseErrors.Add(TEXT("Fatal: Parser returned null AST"));
		Result.SimilarityPercent = 0.0f;
		return Result;
	}
	
	// Step 3: Re-serialize
	FString RoundTripped = AST->ToString();
	Result.RoundTrippedText = NormalizeForComparison(RoundTripped);
	
	// Step 4: Compare
	Result.SimilarityPercent = CompareTexts(Result.OriginalText, Result.RoundTrippedText, Result.Differences);
	Result.bSuccess = (Result.Differences.Num() == 0) && (ParseErrors.Num() == 0);
	
	// Stats
	{
		TArray<FString> OrigLines;
		Result.OriginalText.ParseIntoArrayLines(OrigLines, false);
		Result.OriginalLines = OrigLines.Num();
		
		TArray<FString> RtLines;
		Result.RoundTrippedText.ParseIntoArrayLines(RtLines, false);
		Result.RoundTrippedLines = RtLines.Num();
	}
	
	return Result;
}

// ========== TestDirectory ==========

TArray<FRoundTripResult> FAnimLangRoundTrip::TestDirectory(const FString& DirectoryPath)
{
	TArray<FRoundTripResult> Results;
	
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(DirectoryPath / TEXT("*.animlang")), true, false);
	
	for (const FString& FileName : Files)
	{
		// Skip combined file
		if (FileName.StartsWith(TEXT("_")))
		{
			continue;
		}
		
		FString FullPath = DirectoryPath / FileName;
		FString Content;
		if (FFileHelper::LoadFileToString(Content, *FullPath))
		{
			FString BaseName = FPaths::GetBaseFilename(FileName);
			FRoundTripResult Result = TestString(Content, BaseName);
			Results.Add(Result);
			
			UE_LOG(LogTemp, Log, TEXT("Round-trip [%s]: %s (%.1f%% similar, %d diffs, %d parse errors)"),
				*BaseName,
				Result.bSuccess ? TEXT("PASS") : TEXT("FAIL"),
				Result.SimilarityPercent,
				Result.Differences.Num(),
				Result.ParseErrors.Num());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Failed to read: %s"), *FullPath);
		}
	}
	
	return Results;
}

// ========== GenerateReport ==========

FString FAnimLangRoundTrip::GenerateReport(const TArray<FRoundTripResult>& Results)
{
	FString Report;
	
	int32 PassCount = 0;
	int32 FailCount = 0;
	
	for (const FRoundTripResult& R : Results)
	{
		if (R.bSuccess) PassCount++;
		else FailCount++;
	}
	
	Report += TEXT("====== AnimLang Round-Trip Validation Report ======\n\n");
	Report += FString::Printf(TEXT("Total: %d tests, %d passed, %d failed\n\n"), Results.Num(), PassCount, FailCount);
	
	for (const FRoundTripResult& R : Results)
	{
		Report += FString::Printf(TEXT("--- %s: %s (%.1f%% similar) ---\n"),
			*R.AssetName,
			R.bSuccess ? TEXT("PASS") : TEXT("FAIL"),
			R.SimilarityPercent);
		
		Report += FString::Printf(TEXT("  Original: %d lines | Round-tripped: %d lines\n"),
			R.OriginalLines, R.RoundTrippedLines);
		
		if (R.ParseErrors.Num() > 0)
		{
			Report += FString::Printf(TEXT("  Parse errors (%d):\n"), R.ParseErrors.Num());
			for (const FString& Err : R.ParseErrors)
			{
				Report += TEXT("    ") + Err + TEXT("\n");
			}
		}
		
		if (R.Differences.Num() > 0)
		{
			Report += FString::Printf(TEXT("  Differences (%d):\n"), R.Differences.Num());
			int32 MaxDiffs = FMath::Min(R.Differences.Num(), 20);  // Cap displayed diffs
			for (int32 i = 0; i < MaxDiffs; i++)
			{
				Report += TEXT("    ") + R.Differences[i] + TEXT("\n");
			}
			if (R.Differences.Num() > MaxDiffs)
			{
				Report += FString::Printf(TEXT("    ... and %d more differences\n"), R.Differences.Num() - MaxDiffs);
			}
		}
		
		Report += TEXT("\n");
	}
	
	Report += TEXT("====== End of Report ======\n");
	return Report;
}

// ========== Internal Helpers ==========

FString FAnimLangRoundTrip::NormalizeForComparison(const FString& Text)
{
	// Split into lines
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	
	// Trim trailing whitespace per line, remove trailing empty lines
	while (Lines.Num() > 0 && Lines.Last().TrimEnd().IsEmpty())
	{
		Lines.RemoveAt(Lines.Num() - 1);
	}
	
	// Remove leading empty lines
	while (Lines.Num() > 0 && Lines[0].TrimEnd().IsEmpty())
	{
		Lines.RemoveAt(0);
	}
	
	FString Result;
	for (int32 i = 0; i < Lines.Num(); i++)
	{
		if (i > 0) Result += TEXT("\n");
		Result += Lines[i].TrimEnd();
	}
	
	return Result;
}

float FAnimLangRoundTrip::CompareTexts(const FString& Original, const FString& RoundTripped, TArray<FString>& OutDiffs)
{
	TArray<FString> OrigLines;
	Original.ParseIntoArrayLines(OrigLines, false);
	
	TArray<FString> RtLines;
	RoundTripped.ParseIntoArrayLines(RtLines, false);
	
	int32 MaxLines = FMath::Max(OrigLines.Num(), RtLines.Num());
	if (MaxLines == 0) return 100.0f;
	
	int32 MatchCount = 0;
	int32 CompareLen = FMath::Min(OrigLines.Num(), RtLines.Num());
	
	for (int32 i = 0; i < CompareLen; i++)
	{
		FString OLine = OrigLines[i].TrimEnd();
		FString RLine = RtLines[i].TrimEnd();
		
		if (OLine == RLine)
		{
			MatchCount++;
		}
		else
		{
			// Check if it's just a whitespace difference
			FString OTrimmed = OLine.TrimStartAndEnd();
			FString RTrimmed = RLine.TrimStartAndEnd();
			
			if (OTrimmed == RTrimmed)
			{
				MatchCount++;  // Whitespace-only diff is still a match
			}
			else
			{
				OutDiffs.Add(FString::Printf(TEXT("Line %d:\n  orig: %s\n  rt:   %s"), i + 1, *OLine, *RLine));
			}
		}
	}
	
	// Lines that exist in one but not the other
	if (OrigLines.Num() > RtLines.Num())
	{
		for (int32 i = RtLines.Num(); i < OrigLines.Num(); i++)
		{
			FString OLine = OrigLines[i].TrimEnd();
			if (!OLine.IsEmpty())
			{
				OutDiffs.Add(FString::Printf(TEXT("Line %d: Missing in round-tripped: %s"), i + 1, *OLine));
			}
		}
	}
	else if (RtLines.Num() > OrigLines.Num())
	{
		for (int32 i = OrigLines.Num(); i < RtLines.Num(); i++)
		{
			FString RLine = RtLines[i].TrimEnd();
			if (!RLine.IsEmpty())
			{
				OutDiffs.Add(FString::Printf(TEXT("Line %d: Extra in round-tripped: %s"), i + 1, *RLine));
			}
		}
	}
	
	return (float)MatchCount / (float)MaxLines * 100.0f;
}
