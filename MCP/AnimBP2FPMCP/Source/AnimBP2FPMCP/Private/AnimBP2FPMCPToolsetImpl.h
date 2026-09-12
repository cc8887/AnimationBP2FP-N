// AnimBP2FPMCPToolsetImpl.h - UE 5.8 Toolset Registry adapter

#pragma once

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "AnimBP2FPMCPToolset.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "AnimBP2FPMCPToolsetImpl.generated.h"

/**
 * UE 5.8 Toolset Registry adapter for the AnimBP2FP editor APIs.
 * The Python bridge remains available for compatibility; these functions are
 * the native MCP surface and do not accept arbitrary filesystem paths.
 */
UCLASS(BlueprintType, Hidden)
class ANIMBP2FPMCP_API UAnimBP2FPToolset : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/** Export an AnimBlueprint to AnimLang text. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "AnimBP|Read")
	static FAnimBP2FPMCPTextResult ExportAnimBlueprint(const FString& AssetPath);

	/** Export a BlueprintLisp graph from an AnimBlueprint. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "EventGraph|Read")
	static FAnimBP2FPMCPTextResult ExportEventGraph(
		const FString& AssetPath,
		const FString& GraphName = TEXT("EventGraph"),
		bool bIncludePositions = false,
		bool bStableIds = true);

	/** Read one Blueprint <-> DSL mapping entry. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "AnimBP|Read")
	static FAnimBP2FPMCPMappingResult GetAnimBlueprintSyncState(const FString& AssetPath);

	/** Apply an in-memory Anim/Rig bundle using strict preflight and commit gates. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "AnimBP|Write")
	static FAnimBP2FPMCPBundleResult ApplyAnimBundle(
		const TArray<FAnimBP2FPMCPBundleSource>& Sources,
		const FString& TargetRoot,
		EAnimBP2FPMCPBundleMode Mode = EAnimBP2FPMCPBundleMode::Strict,
		bool bCommitPersistent = true);

	/** Update an existing AnimBlueprint from AnimLang text. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "AnimBP|Write")
	static FAnimBP2FPMCPChangeResult UpdateAnimBlueprint(
		const FString& AssetPath,
		const FString& DSLText,
		bool bSavePackage = true);

	/** Replace a BlueprintLisp graph and optionally compile/save the asset. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EventGraph|Write")
	static FAnimBP2FPMCPChangeResult ReplaceEventGraph(
		const FString& AssetPath,
		const FString& GraphName,
		const FString& DSLText,
		bool bCompile = true,
		bool bSavePackage = true);

	/** Merge-append a BlueprintLisp graph using the current supported reuse semantics. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EventGraph|Write")
	static FAnimBP2FPMCPChangeResult MergeEventGraph(
		const FString& AssetPath,
		const FString& GraphName,
		const FString& DSLText,
		bool bCompile = true,
		bool bSavePackage = true);
};
