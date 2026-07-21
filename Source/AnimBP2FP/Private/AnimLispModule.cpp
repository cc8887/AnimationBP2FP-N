// AnimLispModule.cpp - Shared AnimLang and RigLang module contracts
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLispModule.h"

namespace
{
FString NormalizeAssetPath(const FString& InAssetPath)
{
	FString Result = InAssetPath.TrimStartAndEnd();
	Result.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	while (Result.Contains(TEXT("//")))
	{
		Result.ReplaceInline(TEXT("//"), TEXT("/"), ESearchCase::CaseSensitive);
	}
	if (!Result.IsEmpty() && !Result.StartsWith(TEXT("/")))
	{
		Result.InsertAt(0, TEXT('/'));
	}
	while (Result.Len() > 1 && Result.EndsWith(TEXT("/")))
	{
		Result.LeftChopInline(1, EAllowShrinking::No);
	}
	return Result;
}
}

FAnimLispModuleId FAnimLispModuleId::FromAssetPath(const FString& InAssetPath, EAnimLispModuleKind InKind)
{
	FAnimLispModuleId Result;
	Result.Kind = InKind;
	Result.AssetPath = NormalizeAssetPath(InAssetPath);
	return Result;
}

FString FAnimLispModuleId::ToString() const
{
	const TCHAR* Prefix = Kind == EAnimLispModuleKind::Rig ? TEXT("rig:") : TEXT("anim:");
	return Prefix + AssetPath;
}

bool FAnimLispModuleId::operator==(const FAnimLispModuleId& Other) const
{
	return Kind == Other.Kind && AssetPath == Other.AssetPath;
}

uint32 GetTypeHash(const FAnimLispModuleId& ModuleId)
{
	return HashCombine(::GetTypeHash(static_cast<uint8>(ModuleId.Kind)), FCrc::StrCrc32(*ModuleId.AssetPath));
}

bool FAnimLispTypeRef::operator==(const FAnimLispTypeRef& Other) const
{
	return CPPType == Other.CPPType
		&& CPPTypeObject == Other.CPPTypeObject
		&& ContainerType == Other.ContainerType;
}

bool FAnimLispTypeRef::operator!=(const FAnimLispTypeRef& Other) const
{
	return !(*this == Other);
}
