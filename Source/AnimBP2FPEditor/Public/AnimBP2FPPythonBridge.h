// AnimBP2FPPythonBridge.h - Python-facing editor bridge for AnimBP2FP
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AnimBP2FPPythonBridge.generated.h"

/**
 * Structured result returned to Unreal Python / Blueprint callers.
 */
USTRUCT(BlueprintType)
struct ANIMBP2FPEDITOR_API FAnimBP2FPPythonResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	FString FilePath;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	FString DSLText;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	bool bUsedIncrementalPatch = false;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	bool bSavedPackage = false;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	int32 NumChanges = 0;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	int32 NumPropertyChanges = 0;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	int32 NumStructuralChanges = 0;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	TArray<FString> AppliedOps;

	UPROPERTY(BlueprintReadOnly, Category="AnimBP2FP")
	TArray<FString> Warnings;
};

/**
 * Editor-only bridge exposed to Unreal Python so AI agents can read/write AnimBlueprints
 * through AnimLang without shelling out to commandlets.
 */
UCLASS()
class ANIMBP2FPEDITOR_API UAnimBP2FPPythonBridge : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Export an existing AnimBlueprint asset to AnimLang DSL text.
	 * AnimBlueprintPath accepts either package path or object path, e.g.
	 *   /Game/Foo/ALS_AnimBP
	 *   /Game/Foo/ALS_AnimBP.ALS_AnimBP
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ExportAnimBlueprintToText(const FString& AnimBlueprintPath);

	/**
	 * Export an existing AnimBlueprint asset to an AnimLang file.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ExportAnimBlueprintToFile(const FString& AnimBlueprintPath, const FString& OutputFilePath);

	/**
	 * Import AnimLang DSL text as a new AnimBlueprint.
	 * DestinationFolder should be a content folder such as /Game/AnimLang/Imported.
	 * The created asset name comes from the DSL root form.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ImportAnimBlueprintFromText(const FString& DSLText, const FString& DestinationFolder, bool bSavePackage = true);

	/**
	 * Import an AnimLang file as a new AnimBlueprint.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ImportAnimBlueprintFromFile(const FString& InputFilePath, const FString& DestinationFolder, bool bSavePackage = true);

	/**
	 * Update an existing AnimBlueprint from AnimLang DSL text.
	 * AnimBlueprintPath accepts either package path or object path.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult UpdateAnimBlueprintFromText(const FString& AnimBlueprintPath, const FString& DSLText, bool bSavePackage = true);

	/**
	 * Update an existing AnimBlueprint from an AnimLang file.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult UpdateAnimBlueprintFromFile(const FString& AnimBlueprintPath, const FString& InputFilePath, bool bSavePackage = true);

	/**
	 * Export the EventGraph (or another graph) to BlueprintLisp text.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ExportEventGraphToText(
		const FString& AnimBlueprintPath,
		const FString& GraphName = TEXT("EventGraph"),
		bool bIncludePositions = false,
		bool bStableIds = true);

	/**
	 * Export the EventGraph (or another graph) to a BlueprintLisp file.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ExportEventGraphToFile(
		const FString& AnimBlueprintPath,
		const FString& OutputFilePath,
		const FString& GraphName = TEXT("EventGraph"),
		bool bIncludePositions = false,
		bool bStableIds = true);

	/**
	 * Import BlueprintLisp text into the EventGraph (or another graph) of an AnimBlueprint.
	 * Current implementation uses BlueprintLisp ReplaceGraph semantics.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ImportEventGraphFromText(
		const FString& AnimBlueprintPath,
		const FString& GraphName,
		const FString& DSLText,
		bool bCompile = true,
		bool bSavePackage = true);

	/**
	 * Import a BlueprintLisp file into the EventGraph (or another graph) of an AnimBlueprint.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ImportEventGraphFromFile(
		const FString& AnimBlueprintPath,
		const FString& GraphName,
		const FString& InputFilePath,
		bool bCompile = true,
		bool bSavePackage = true);

	/**
	 * Update the EventGraph (or another graph) of an AnimBlueprint from BlueprintLisp text.
	 * Current implementation intentionally falls back to ReplaceGraph import until
	 * BlueprintLisp semantic update is implemented.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult UpdateEventGraphFromText(
		const FString& AnimBlueprintPath,
		const FString& GraphName,
		const FString& DSLText,
		bool bCompile = true,
		bool bSavePackage = true);

	/**
	 * Update the EventGraph (or another graph) of an AnimBlueprint from a BlueprintLisp file.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult UpdateEventGraphFromFile(
		const FString& AnimBlueprintPath,
		const FString& GraphName,
		const FString& InputFilePath,
		bool bCompile = true,
		bool bSavePackage = true);


	// ========== Mapping Registry ==========

	/**
	 * Query the AnimBlueprint <-> DSL mapping table.
	 * Returns all entries as a JSON string: array of objects with keys:
	 *   blueprint_path, dsl_file_path, category, state, has_blueprint, has_dsl
	 * State values: "Synced", "BPOnly", "DSLOnly", "OutOfSync"
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult GetMappingTable();

	/**
	 * Look up a single mapping entry by AnimBlueprint path.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult FindMappingByBlueprint(const FString& AnimBlueprintPath);

	/**
	 * Convert an AnimBlueprint package path to its corresponding DSL file path.
	 *   /Game/Characters/ALS/ALS_Npc -> {Project}/Saved/BP2DSL/AnimBP/Characters/ALS/ALS_Npc.animlang
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult AnimBlueprintPathToDSLPath(const FString& AnimBlueprintPath);

	// ========== Validation ==========

	/**
	 * Run round-trip validation on a single AnimBlueprint:
	 *   Export -> Parse -> ToString -> diff
	 * Returns fidelity percentage and any diff lines in Warnings.
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ValidateAnimBlueprintRoundTrip(const FString& AnimBlueprintPath);

	// ========== Stub Export ==========

	/**
	 * Export all UAnimGraphNode type definitions to a stub file.
	 * Outputs Typed Racket format with node signatures for Lint/validation.
	 * Default path: {Project}/Saved/BP2DSL/AnimBP/animlang-nodes-generated.rkt
	 */
	UFUNCTION(BlueprintCallable, Category="AnimBP2FP|Python")
	static FAnimBP2FPPythonResult ExportStub(const FString& OutputFilePath = TEXT(""));
};
