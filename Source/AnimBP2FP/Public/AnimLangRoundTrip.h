// AnimLangRoundTrip.h - Round-trip Validation (Export → Parse → ToString → Compare)
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangAST.h"

/**
 * Result of a single round-trip test
 */
struct ANIMBP2FP_API FRoundTripResult
{
	FString AssetName;
	bool bSuccess;
	
	// Source text (from Export)
	FString OriginalText;
	
	// Re-serialized text (from Parse → ToString)
	FString RoundTrippedText;
	
	// Diff details (if different)
	TArray<FString> Differences;
	
	// Parse errors
	TArray<FString> ParseErrors;
	
	// Stats
	int32 OriginalLines;
	int32 RoundTrippedLines;
	float SimilarityPercent;  // 0-100
};

/**
 * AnimLang Round-trip Validator
 * 
 * Verifies that Export → Parse → ToString produces identical output.
 * This validates:
 *   1. Tokenizer handles all DSL constructs
 *   2. Parser reconstructs the correct AST
 *   3. AST → string serialization is deterministic
 */
class ANIMBP2FP_API FAnimLangRoundTrip
{
public:
	/**
	 * Run round-trip test on a DSL string
	 * @param DSLCode  The original DSL text
	 * @param AssetName  Name for the test result
	 * @return Test result with comparison details
	 */
	static FRoundTripResult TestString(const FString& DSLCode, const FString& AssetName = TEXT("test"));
	
	/**
	 * Run round-trip test on all .animlang files in a directory
	 * @param DirectoryPath  Path to directory containing .animlang files
	 * @return Array of test results
	 */
	static TArray<FRoundTripResult> TestDirectory(const FString& DirectoryPath);
	
	/**
	 * Generate a human-readable report from test results
	 * @param Results  Array of round-trip test results
	 * @return Formatted report string
	 */
	static FString GenerateReport(const TArray<FRoundTripResult>& Results);
	
private:
	/**
	 * Compare two DSL strings line by line (ignoring whitespace-only diffs)
	 * @param Original  The original text
	 * @param RoundTripped  The re-serialized text
	 * @param OutDiffs  Receives diff descriptions
	 * @return Similarity percentage (0-100)
	 */
	static float CompareTexts(const FString& Original, const FString& RoundTripped, TArray<FString>& OutDiffs);
	
	/**
	 * Normalize a DSL string for comparison
	 * - Trim trailing whitespace per line
	 * - Normalize line endings
	 * - Remove blank lines at start/end
	 */
	static FString NormalizeForComparison(const FString& Text);
};
