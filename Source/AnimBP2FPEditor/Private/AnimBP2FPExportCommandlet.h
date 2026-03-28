// AnimBP2FPExportCommandlet.h - Commandlet for testing AnimBP export
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimBP2FPExportCommandlet.generated.h"

/**
 * Commandlet to export Animation Blueprints to AnimLang DSL
 * Usage: UnrealEditor.exe <project> -run=AnimBP2FPExport
 */
UCLASS()
class UAnimBP2FPExportCommandlet : public UCommandlet
{
	GENERATED_BODY()
	
public:
	UAnimBP2FPExportCommandlet();
	
	virtual int32 Main(const FString& Params) override;
};
