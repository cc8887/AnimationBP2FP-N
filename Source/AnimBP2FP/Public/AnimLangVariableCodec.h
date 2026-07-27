// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "AnimLangAST.h"
#include "EdGraph/EdGraphPin.h"

class ANIMBP2FP_API FAnimLangVariableCodec
{
public:
	static bool BuildPinType(const FVariableDef& Variable, FEdGraphPinType& OutPinType, FString& OutError);
};

#endif
