// RigLangImporter.h - Staging importer for Rig hierarchy and member variables
#pragma once

#include "CoreMinimal.h"
#include "AnimLangDiagnostics.h"
#include "RigLangAST.h"

class UControlRigBlueprint;

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
	static FRigLangImportResult Import(
		const FRigModuleAST& Module,
		const FRigLangImportOptions& Options);
};
