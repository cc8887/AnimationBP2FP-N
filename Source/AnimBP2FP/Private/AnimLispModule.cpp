// AnimLispModule.cpp - Shared AnimLang and RigLang module contracts
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimLispModule.h"
#include "AnimBP2FPVersionCompat.h"

FString AnimLispStableRuntimeSymbol(const FString& Value)
{
	FString Result;
	for (const TCHAR Character : Value)
	{
		if (FChar::IsAlnum(Character) || Character == TEXT('_')) Result.AppendChar(Character);
	}
	if (!Result.IsEmpty() && FChar::IsDigit(Result[0])) Result = TEXT("_") + Result;
	return Result;
}

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
		Result.LeftChopInline(1, ANIMBP2FP_NO_SHRINKING);
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

void FAnimLispTypeRef::Canonicalize()
{
	CPPType.TrimStartAndEndInline();
	CPPTypeObject.TrimStartAndEndInline();
	ContainerType.TrimStartAndEndInline();
	if (CPPTypeObject.Equals(TEXT("None"), ESearchCase::IgnoreCase)) CPPTypeObject.Reset();
	if (ContainerType.Equals(TEXT("None"), ESearchCase::IgnoreCase)) ContainerType.Reset();
}

bool FAnimLispTypeRef::operator==(const FAnimLispTypeRef& Other) const
{
	FAnimLispTypeRef Left = *this;
	FAnimLispTypeRef Right = Other;
	Left.Canonicalize();
	Right.Canonicalize();
	return Left.CPPType == Right.CPPType
		&& Left.CPPTypeObject == Right.CPPTypeObject
		&& Left.ContainerType == Right.ContainerType;
}

bool FAnimLispTypeRef::operator!=(const FAnimLispTypeRef& Other) const
{
	return !(*this == Other);
}
