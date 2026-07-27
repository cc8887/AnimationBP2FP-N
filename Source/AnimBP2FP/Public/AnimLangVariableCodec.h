// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "AnimLangAST.h"
#include "EdGraph/EdGraphPin.h"

class FProperty;
class UAnimBlueprint;

class ANIMBP2FP_API FAnimLangVariableCodec
{
public:
	static bool BuildPinType(const FVariableDef& Variable, FEdGraphPinType& OutPinType, FString& OutError);
	static bool ExportMapEntries(const UAnimBlueprint& Blueprint, FVariableDef& InOutVariable, FString& OutError);

private:
	static FString ExportPropertyExpression(const FProperty& Property, const void* Value);
};

#endif
