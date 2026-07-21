// RigLangExporter.h - Control Rig to RigLang export
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RigLangAST.h"

class UControlRigBlueprint;

struct ANIMBP2FP_API FRigLangExportOptions
{
	bool bStrict = true;
};

struct ANIMBP2FP_API FRigLangExportCoverage
{
	int32 ModelTotal = 0;
	int32 NodeTotal = 0;
	int32 PinTotal = 0;
	int32 LinkTotal = 0;
	TSet<FString> VisitedModels;
	TSet<FString> VisitedNodes;
	TSet<FString> VisitedPins;
	TSet<FString> VisitedLinks;
	TSet<FString> ConnectedPins;
	TSet<FString> LossyOrUnsupportedNodes;
	TMap<FString, FString> Reasons;
};

struct ANIMBP2FP_API FRigLangExportResult
{
	bool bSuccess = false;
	TSharedPtr<FRigModuleAST> Module;
	FRigLangExportCoverage Coverage;
	TArray<FString> Errors;
};

class ANIMBP2FP_API FRigLangExporter
{
public:
	/** Computes the canonical, UTF-8 SHA-256 content hash used by module imports. */
	static FString ComputeContentHash(const FString& CanonicalHashInput);
	/** Derives a stable RFC-4122-shaped graph GUID from length-delimited module and graph identities. */
	static FString ComputeDeterministicEditorGuid(
		const FString& ModuleIdentity,
		const FString& GraphIdentity);
	static bool ValidateStrictCoverage(
		const FRigLangExportCoverage& Coverage,
		TArray<FString>& OutErrors);
	/** Resolves typed Rig calls and synthesizes deterministic external Rig imports. */
	static void NormalizeFunctionCallsAndImports(FRigModuleAST& Module);

	static FRigLangExportResult Export(
		UControlRigBlueprint* Blueprint,
		const FRigLangExportOptions& Options = FRigLangExportOptions());
};
