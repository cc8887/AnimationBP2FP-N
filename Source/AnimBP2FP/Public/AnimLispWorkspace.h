// AnimLispWorkspace.h - Cross-file AnimLang and RigLang symbol workspace
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimLangDiagnostics.h"
#include "AnimLispModule.h"

struct ANIMBP2FP_API FAnimLispSymbolId
{
	FAnimLispModuleId Module;
	FString QualifiedName;
	EAnimLispSymbolKind Kind = EAnimLispSymbolKind::AnimDefine;

	bool operator==(const FAnimLispSymbolId& Other) const;
	bool operator!=(const FAnimLispSymbolId& Other) const { return !(*this == Other); }
};

ANIMBP2FP_API uint32 GetTypeHash(const FAnimLispSymbolId& SymbolId);

struct ANIMBP2FP_API FAnimLispDefinition
{
	FAnimLispSymbolId Id;
	EAnimLispCapability Capability = EAnimLispCapability::DefinitionOnly;
	FAnimLispTypeRef TypeSignature;
	FAnimLangSourceLoc Location;
	FString Guid;
};

struct ANIMBP2FP_API FAnimLispReference
{
	FAnimLispSymbolId Target;
	FAnimLangSourceLoc Location;
};

struct ANIMBP2FP_API FAnimLispCompletion
{
	FAnimLispSymbolId Id;
	EAnimLispCapability Capability = EAnimLispCapability::DefinitionOnly;
	FAnimLispTypeRef TypeSignature;
	FAnimLangSourceLoc Location;
	FString Guid;
};

class ANIMBP2FP_API FAnimLispWorkspace
{
public:
	FAnimLispWorkspace();
	~FAnimLispWorkspace();

	FAnimLispWorkspace(const FAnimLispWorkspace&) = delete;
	FAnimLispWorkspace& operator=(const FAnimLispWorkspace&) = delete;

	void AddSource(const FString& Path, const FString& Source);
	bool Build(FAnimLangDiagnostics& OutDiag);

	// The returned pointer is invalidated by the next Build call.
	const FAnimLispDefinition* FindDefinition(
		const FString& FromFile,
		const FString& QualifiedName) const;
	TArray<FAnimLispReference> FindReferences(const FAnimLispSymbolId& Symbol) const;
	TArray<FAnimLispCompletion> Complete(
		const FString& FromFile,
		EAnimLispCapability Required) const;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
