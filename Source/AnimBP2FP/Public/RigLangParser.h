// RigLangParser.h - Parser for canonical RigLang modules
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RigLangAST.h"

struct ANIMBP2FP_API FRigLangParseError
{
	FString Message;
	FAnimLangSourceLoc Location;

	FString ToString() const
	{
		return FString::Printf(TEXT("%s: %s"), *Location.ToString(), *Message);
	}
};

class ANIMBP2FP_API FRigLangParser
{
public:
	static TSharedPtr<FRigModuleAST> Parse(
		const FString& Source,
		const FString& SourceFile,
		TArray<FRigLangParseError>& OutErrors);
};
