// AnimBP2FPImportCommandlet.h - DSL Import commandlet
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimBP2FPImportCommandlet.generated.h"

/**
 * Commandlet to import .animlang DSL files into Animation Blueprints
 * Usage: UnrealEditor.exe <project> -run=AnimBP2FPImport -Bundle=<workspace> [-OutDir=<package_path>] [-Legacy]
 * 
 * Options:
 *   -file=<path>        Import a specific .animlang file
 *   -outdir=<path>      Package path for output (default: /Game/AnimBP2FP/Imported)
 *   -test               Run import round-trip test: Import → Export → Compare
 *   -update             Run update test: Import → Modify → UpdateBlueprint → Verify
 *
 * Without -file, imports all .animlang files from <ProjectDir>/AnimLang/Exported/
 */
UCLASS()
class UAnimBP2FPImportCommandlet : public UCommandlet
{
	GENERATED_BODY()
	
public:
	UAnimBP2FPImportCommandlet();
	
	virtual int32 Main(const FString& Params) override;
	
private:
	/** Import a single .animlang file and return the created blueprint */
	UAnimBlueprint* ImportFile(const FString& FilePath, const FString& OutputPackagePath, FString& OutError);
	
	/** Run import round-trip test on a single file */
	bool RunImportRoundTrip(const FString& FilePath, const FString& OutputPackagePath, FString& OutReport);
	
	/** Run UpdateBlueprint test on a single file:
	 *  1. Import original DSL → Blueprint
	 *  2. Apply a simulated edit to the DSL (property changes)
	 *  3. Call UpdateBlueprint with the modified DSL
	 *  4. Export the updated blueprint → DSL
	 *  5. Compare with the modified DSL
	 */
	bool RunUpdateTest(const FString& FilePath, const FString& OutputPackagePath, FString& OutReport);
	
	/** Apply a simulated property edit to DSL code (for testing UpdateBlueprint) */
	FString ApplySimulatedEdit(const FString& OriginalDSL);
};
