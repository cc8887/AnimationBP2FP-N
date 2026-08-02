#pragma once

#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Commandlets/Commandlet.h"
#include "RigLangDiffer.h"
#include "RigLangImportCommandlet.generated.h"

class UControlRigBlueprint;

struct FRigLangTransientRoundTripResult
{
	bool bSuccess = false;
	int32 ModelCount = 0;
	int32 NodeCount = 0;
	int32 LinkCount = 0;
	int32 HierarchyCount = 0;
	int32 CompileErrorCount = 0;
	FString ArtifactDirectory;
	FRigLangDiffResult Diff;
	TArray<FString> Diagnostics;
	TAnimBP2FPObjectPtr<UControlRigBlueprint> ImportedBlueprint = nullptr;
};

namespace RigLangRoundTrip
{
	FRigModuleAST BuildComparableModule(
		const FRigModuleAST& SourceModule,
		const FRigModuleAST& ReExportedModule);

	FRigLangTransientRoundTripResult RunTransient(
		UControlRigBlueprint* SourceBlueprint,
		const FString& RunId);

#if WITH_DEV_AUTOMATION_TESTS
	void SetArtifactWriteFailureForTest(const FString& ArtifactName);
#endif
}

UCLASS()
class URigLangImportCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	URigLangImportCommandlet();
	virtual int32 Main(const FString& Params) override;
};
