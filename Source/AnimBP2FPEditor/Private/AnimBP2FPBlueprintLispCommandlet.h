// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// AnimBP2FPBlueprintLispCommandlet.h
//
// Commandlet: Test EventGraph export via BlueprintLisp plugin
//
// Usage:
//   UnrealEditor.exe <project> -run=AnimBP2FPBlueprintLisp
//   UnrealEditor.exe <project> -run=AnimBP2FPBlueprintLisp -bp=/Game/MyBP
//   UnrealEditor.exe <project> -run=AnimBP2FPBlueprintLisp -graph=EventGraph
//   UnrealEditor.exe <project> -run=AnimBP2FPBlueprintLisp -roundtrip
//
// Options:
//   -bp=<AssetPath>    Export a specific AnimBlueprint (otherwise all)
//   -graph=<Name>      Graph name (default: EventGraph)
//   -roundtrip         Also validate parse round-trip of exported DSL
//   -outdir=<path>     Output directory (default: <ProjectDir>/AnimLang/EventGraph)
//   -nowrite           Print to log only, don't write files

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimBP2FPBlueprintLispCommandlet.generated.h"

UCLASS()
class UAnimBP2FPBlueprintLispCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAnimBP2FPBlueprintLispCommandlet();
	virtual int32 Main(const FString& Params) override;

private:
	struct FTestResult
	{
		FString AssetName;
		FString AssetPath;
		FString GraphName;
		bool    bExportOk     = false;
		bool    bRoundTripOk  = false;
		int32   EventCount    = 0;
		int32   LispLineCount = 0;
		FString Error;
		FString RoundTripError;
	};

	/** Export the EventGraph of one AnimBlueprint and return test result */
	FTestResult ExportOne(
		class UAnimBlueprint* AnimBP,
		const FString&         GraphName,
		bool                   bRoundTrip,
		bool                   bWriteFile,
		const FString&         OutputDir);

	/** Validate parse round-trip: Parse(DSL).ToString() == DSL (minified) */
	bool ValidateRoundTrip(const FString& LispCode, FString& OutError);

	/** Write report to file and log summary */
	void WriteReport(const TArray<FTestResult>& Results, const FString& OutputDir);
};
