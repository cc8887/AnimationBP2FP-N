#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AnimLangDiagnostics.h"
#include "AnimLispLintCommandlet.generated.h"

namespace AnimBP2FPCommandlets
{
	FString SerializeLintDiagnostics(
		const FString& WorkspaceRoot,
		int32 SourceCount,
		const FAnimLangDiagnostics& Diagnostics);
	int32 LintExitCode(const FAnimLangDiagnostics& Diagnostics);
	void CollectStrictCoverageDiagnostics(
		const FString& SourcePath,
		const FString& Source,
		FAnimLangDiagnostics& OutDiagnostics);
}

UCLASS()
class UAnimLispLintCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UAnimLispLintCommandlet();
	virtual int32 Main(const FString& Params) override;
};
