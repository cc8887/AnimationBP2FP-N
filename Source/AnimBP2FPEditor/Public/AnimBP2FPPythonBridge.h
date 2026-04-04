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
	 * Current scope is export only; import is not implemented yet.
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
};
