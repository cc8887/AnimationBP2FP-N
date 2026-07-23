// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RigLangAST.h"

struct ANIMBP2FP_API FRigLangDifference
{
	FString Path;
	FString OldValue;
	FString NewValue;
};

struct ANIMBP2FP_API FRigLangDiffResult
{
	TArray<FRigLangDifference> Differences;

	bool IsEmpty() const { return Differences.IsEmpty(); }
	FString ToJson() const;
};

class ANIMBP2FP_API FRigLangDiffer
{
public:
	static FRigLangDiffResult Diff(const FRigModuleAST& OldModule, const FRigModuleAST& NewModule);
};
