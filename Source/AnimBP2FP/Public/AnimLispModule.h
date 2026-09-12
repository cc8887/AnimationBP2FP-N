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
	bool operator!=(const FAnimLispModuleId& Other) const { return !(*this == Other); }
};

ANIMBP2FP_API uint32 GetTypeHash(const FAnimLispModuleId& ModuleId);
ANIMBP2FP_API FString AnimLispStableRuntimeSymbol(const FString& Value);

struct ANIMBP2FP_API FAnimLispImport
{
	FAnimLispModuleId Target;
	FString Alias;
	FString ExpectedHash;
	FAnimLangSourceLoc Location;
	/** True only when the parser synthesized this import from an archived asset reference. */
	bool bLegacyExternal = false;
};

/** Exact Unreal type identity shared by future Anim and Rig symbols. */
struct ANIMBP2FP_API FAnimLispTypeRef
{
	FString CPPType;
	FString CPPTypeObject;
	FString ContainerType;

	void Canonicalize();
	bool operator==(const FAnimLispTypeRef& Other) const;
	bool operator!=(const FAnimLispTypeRef& Other) const;
};
