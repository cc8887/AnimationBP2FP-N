// AnimBP2FPMCPToolset.h - reflected result schemas for AnimBP2FP MCP

#pragma once

#include "CoreMinimal.h"

#include "AnimBP2FPMCPToolset.generated.h"

UENUM(BlueprintType)
enum class EAnimBP2FPMCPBundleMode : uint8
{
	Strict,
	Legacy
};

USTRUCT(BlueprintType)
struct ANIMBP2FPMCP_API FAnimBP2FPMCPDiagnostic
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Severity;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Category;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString SourceFile;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	int32 Line = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	int32 Column = 0;
};

USTRUCT(BlueprintType)
struct ANIMBP2FPMCP_API FAnimBP2FPMCPTextResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Error;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Text;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	TArray<FString> Warnings;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bSavedPackage = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bMutationStarted = false;
};

USTRUCT(BlueprintType)
struct ANIMBP2FPMCP_API FAnimBP2FPMCPMappingResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Error;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString DSLFilePath;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Category;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString State;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bBlueprintExists = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bDSLExists = false;
};

USTRUCT(BlueprintType)
struct ANIMBP2FPMCP_API FAnimBP2FPMCPChangeResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Error;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString AssetPath;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bSavedPackage = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bMutationStarted = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bUsedIncrementalPatch = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	int32 NumChanges = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	int32 NumPropertyChanges = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	int32 NumStructuralChanges = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	TArray<FString> AppliedOps;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	TArray<FString> Warnings;
};

USTRUCT(BlueprintType)
struct ANIMBP2FPMCP_API FAnimBP2FPMCPBundleSource
{
	GENERATED_BODY()

	/** Logical source name, normally ending in .animlang or .riglang. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AnimBP2FP|MCP")
	FString SourceFile;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AnimBP2FP|MCP")
	FString Source;
};

USTRUCT(BlueprintType)
struct ANIMBP2FPMCP_API FAnimBP2FPMCPBundleResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	FString Error;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bMutationStarted = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	bool bPersisted = false;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	TArray<FString> ModulePlan;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	TArray<FString> AssetPaths;

	UPROPERTY(BlueprintReadOnly, Category = "AnimBP2FP|MCP")
	TArray<FAnimBP2FPMCPDiagnostic> Diagnostics;
};
