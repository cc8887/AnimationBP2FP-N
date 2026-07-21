// AnimLispModule.h - Shared AnimLang and RigLang module contracts
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangDiagnostics.h"

enum class EAnimLispModuleKind : uint8
{
	Anim,
	Rig
};

enum class EAnimLispSymbolKind : uint8
{
	AnimDefine,
	AnimVariable,
	RigEntry,
	RigFunction,
	RigVariable,
	RigHierarchyElement
};

enum class EAnimLispCapability : uint8
{
	AnimRuntimeReference,
	RigCall,
	DefinitionOnly
};

struct ANIMBP2FP_API FAnimLispModuleId
{
	EAnimLispModuleKind Kind = EAnimLispModuleKind::Anim;
	FString AssetPath;

	static FAnimLispModuleId FromAssetPath(const FString& InAssetPath, EAnimLispModuleKind InKind);
	FString ToString() const;
	bool operator==(const FAnimLispModuleId& Other) const;
};

ANIMBP2FP_API uint32 GetTypeHash(const FAnimLispModuleId& ModuleId);

struct ANIMBP2FP_API FAnimLispImport
{
	FAnimLispModuleId Target;
	FString Alias;
	FString ExpectedHash;
	FAnimLangSourceLoc Location;
};

/** Exact Unreal type identity shared by future Anim and Rig symbols. */
struct ANIMBP2FP_API FAnimLispTypeRef
{
	FString CPPType;
	FString CPPTypeObject;
	FString ContainerType;

	bool operator==(const FAnimLispTypeRef& Other) const;
	bool operator!=(const FAnimLispTypeRef& Other) const;
};
