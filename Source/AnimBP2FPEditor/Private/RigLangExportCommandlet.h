#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RigLangExportCommandlet.generated.h"

namespace AnimBP2FPCommandlets
{
	struct FBundleOutput
	{
		FString TargetPath;
		FString Content;
	};
	bool ParseAssetPath(const FString& Params, FString& OutAssetPath, FString& OutError);
	bool ParseAnimExportParams(const FString& Params, FString& OutAssetPath, bool& bOutIncludeRigModules, FString& OutError);
	bool ValidateAssetRoot(const FString& AssetRoot, FString& OutError);
	bool ParseWorkspace(const FString& Params, FString& OutWorkspace, FString& OutError);
	bool PrepareOutputForExport(const FString& OutputPath, FString& OutError);
	bool WriteFileAtomically(const FString& OutputPath, const FString& Content, FString& OutError);
	bool ValidateMappedOutputPath(const FString& OutputPath, FString& OutError);
	void CollectSafeBundleRevokePaths(
		const FString& ManifestPath,
		const FString& CurrentAnimTarget,
		TSet<FString>& OutPaths);
	void CleanupBundleTargets(const TSet<FString>& Paths);
	bool ValidateUniqueOutputPaths(const TArray<FString>& OutputPaths, FString& OutError);
	bool ValidateBundleManifestJson(const FString& Json, FString& OutError);
	bool ValidateBundleOutputHashes(const FString& Json, FString& OutError);
	bool CommitBundleAtomically(
		const TArray<FBundleOutput>& Outputs,
		const FString& ManifestPath,
		const FString& ManifestContent,
		FString& OutError);
	bool CommitBundleAtomically(
		const TArray<FBundleOutput>& Outputs,
		const FString& ManifestPath,
		const FString& ManifestContent,
		const TSet<FString>& PriorBundleTargets,
		FString& OutError);
}

UCLASS()
class URigLangExportCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	URigLangExportCommandlet();
	virtual int32 Main(const FString& Params) override;
};
