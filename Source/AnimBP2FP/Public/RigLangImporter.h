// RigLangImporter.h - Staging importer for Rig hierarchy and member variables
#pragma once

#include "CoreMinimal.h"
#include "AnimLangDiagnostics.h"
#include "RigLangAST.h"

class UControlRigBlueprint;
class URigVMController;
class URigVMNode;

struct ANIMBP2FP_API FRigLangImportOptions
{
	FString TargetPackage;
	bool bTransient = true;
	bool bStrict = true;
};

struct ANIMBP2FP_API FRigLangImportResult
{
	TObjectPtr<UControlRigBlueprint> Blueprint = nullptr;
	FAnimLangDiagnostics Diagnostics;
	bool bCompiled = false;
};

class ANIMBP2FP_API FRigLangImporter
{
public:
	/** Canonical typed pin default; returns the original text when strict typed parsing is unavailable. */
	static FString BuildCanonicalPinDefault(const FRigPinAST& Pin);
	/** Canonical hierarchy/member-variable snapshot with representation-only transform differences normalized. */
	static FString BuildHierarchyVariableSemanticSnapshot(
		const FRigModuleAST& Value,
		const FRigModuleAST& SourceIdentity);
	/** Canonical graph snapshot with generated RigVM identities normalized against the source module. */
	static FString BuildGraphSemanticSnapshot(
		const FRigModuleAST& Value,
		const FRigModuleAST& SourceIdentity);
	/** Logical graph identities include the complete owner ancestry; colliding identities are reported. */
	static TMap<FString, FString> BuildGraphSemanticTokens(
		const FRigModuleAST& Value,
		TSet<FString>* OutCollidingTokens = nullptr);
	static FRigLangImportResult Import(
		const FRigModuleAST& Module,
		const FRigLangImportOptions& Options);
#if WITH_DEV_AUTOMATION_TESTS
	static bool ResolveTemplateNodeForTest(
		URigVMController* Controller,
		URigVMNode*& Node,
		const FRigNodeAST& Source,
		FRigLangImportResult& Result);
#endif
};
