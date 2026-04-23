// AnimBP2FPRoundTripCommandlet.h - Round-trip validation commandlet
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimBP2FPRoundTripCommandlet.generated.h"

/**
 * Commandlet to run round-trip validation on exported .animlang files
 * Usage: UnrealEditor.exe <project> -run=AnimBP2FPRoundTrip
 * 
 * Tests: Export → Parse → ToString → Compare
 * Reads from: <ProjectDir>/AnimLang/Exported/*.animlang
 * Writes report to: <ProjectDir>/AnimLang/Exported/round_trip_report.txt
 */
UCLASS()
class UAnimBP2FPRoundTripCommandlet : public UCommandlet
{
	GENERATED_BODY()
	
public:
	UAnimBP2FPRoundTripCommandlet();
	
	virtual int32 Main(const FString& Params) override;
};
