// AnimBP2FPMCPToolset.cpp - UE 5.8 MCP toolset for AnimBP2FP

#include "AnimBP2FPMCPToolset.h"
#include "AnimBP2FPVersionCompat.h"

#if ANIMBP2FP_HAS_MODERN_RIGVM_AUTHORING

#include "AnimBP2FPMCPToolsetImpl.h"
#include "AnimBP2FPPythonBridge.h"
#include "AnimBPImporter.h"
#include "AnimLangDiagnostics.h"
#include "FBP2FPMappingRegistry.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace AnimBP2FPMCP
{
	static FString DiagnosticSeverity(const EAnimLangDiagSeverity Severity)
	{
		switch (Severity)
		{
		case EAnimLangDiagSeverity::Error: return TEXT("error");
		case EAnimLangDiagSeverity::Warning: return TEXT("warning");
		case EAnimLangDiagSeverity::Info: return TEXT("info");
		case EAnimLangDiagSeverity::Hint: return TEXT("hint");
		}
		return TEXT("unknown");
	}

	static FString DiagnosticCategory(const EAnimLangDiagCategory Category)
	{
		switch (Category)
		{
		case EAnimLangDiagCategory::Lex: return TEXT("lex");
		case EAnimLangDiagCategory::Parse: return TEXT("parse");
		case EAnimLangDiagCategory::Type: return TEXT("type");
		case EAnimLangDiagCategory::Semantic: return TEXT("semantic");
		case EAnimLangDiagCategory::Import: return TEXT("import");
		case EAnimLangDiagCategory::RoundTrip: return TEXT("roundtrip");
		case EAnimLangDiagCategory::Module: return TEXT("module");
		case EAnimLangDiagCategory::Capability: return TEXT("capability");
		}
		return TEXT("unknown");
	}

	static FString MappingState(const EBP2FPSyncState State)
	{
		switch (State)
		{
		case EBP2FPSyncState::Synced: return TEXT("Synced");
		case EBP2FPSyncState::BPOnly: return TEXT("BPOnly");
		case EBP2FPSyncState::DSLOnly: return TEXT("DSLOnly");
		case EBP2FPSyncState::OutOfSync: return TEXT("OutOfSync");
		}
		return TEXT("Unknown");
	}

	static FString NormalizePackagePath(const FString& InAssetPath)
	{
		FString Path = InAssetPath.TrimStartAndEnd();
		Path.ReplaceInline(TEXT("'"), TEXT(""));
		if (Path.Contains(TEXT(".")))
		{
			const FString PackagePath = FPackageName::ObjectPathToPackageName(Path);
			if (!PackagePath.IsEmpty())
			{
				Path = PackagePath;
			}
		}
		return Path;
	}

	static FAnimBP2FPMCPTextResult FromTextResult(const FAnimBP2FPPythonResult& InResult)
	{
		FAnimBP2FPMCPTextResult Result;
		Result.bSuccess = InResult.bSuccess;
		Result.Error = InResult.bSuccess ? FString() : InResult.Message;
		Result.AssetPath = InResult.AssetPath;
		Result.Text = InResult.DSLText;
		Result.Warnings = InResult.Warnings;
		Result.bSavedPackage = InResult.bSavedPackage;
		return Result;
	}

	static FAnimBP2FPMCPChangeResult FromChangeResult(
		const FAnimBP2FPPythonResult& InResult,
		const bool bSaveRequested)
	{
		FAnimBP2FPMCPChangeResult Result;
		Result.bSuccess = InResult.bSuccess && (!bSaveRequested || InResult.bSavedPackage);
		Result.Error = InResult.bSuccess
			? ((!bSaveRequested || InResult.bSavedPackage) ? FString() : InResult.Message)
			: InResult.Message;
		Result.AssetPath = InResult.AssetPath;
		Result.bSavedPackage = InResult.bSavedPackage;
		Result.bMutationStarted = InResult.bSuccess;
		Result.bUsedIncrementalPatch = InResult.bUsedIncrementalPatch;
		Result.NumChanges = InResult.NumChanges;
		Result.NumPropertyChanges = InResult.NumPropertyChanges;
		Result.NumStructuralChanges = InResult.NumStructuralChanges;
		Result.AppliedOps = InResult.AppliedOps;
		Result.Warnings = InResult.Warnings;
		return Result;
	}

	static FAnimBP2FPMCPDiagnostic ToDiagnostic(const FAnimLangDiagnostic& InDiagnostic)
	{
		FAnimBP2FPMCPDiagnostic Result;
		Result.Severity = DiagnosticSeverity(InDiagnostic.Severity);
		Result.Category = DiagnosticCategory(InDiagnostic.Category);
		Result.Message = InDiagnostic.Message;
		Result.SourceFile = InDiagnostic.Location.SourceFile;
		Result.Line = InDiagnostic.Location.Line;
		Result.Column = InDiagnostic.Location.Column;
		return Result;
	}

	static bool IsSafeSourceName(const FString& SourceFile)
	{
		return !SourceFile.IsEmpty()
			&& FPaths::IsRelative(SourceFile)
			&& !SourceFile.Contains(TEXT(".."))
			&& !SourceFile.Contains(TEXT(":"))
			&& (SourceFile.EndsWith(TEXT(".animlang")) || SourceFile.EndsWith(TEXT(".riglang")));
	}
}

FAnimBP2FPMCPTextResult UAnimBP2FPToolset::ExportAnimBlueprint(const FString& AssetPath)
{
	return AnimBP2FPMCP::FromTextResult(
		UAnimBP2FPPythonBridge::ExportAnimBlueprintToText(AssetPath));
}

FAnimBP2FPMCPTextResult UAnimBP2FPToolset::ExportEventGraph(
	const FString& AssetPath,
	const FString& GraphName,
	const bool bIncludePositions,
	const bool bStableIds)
{
	return AnimBP2FPMCP::FromTextResult(
		UAnimBP2FPPythonBridge::ExportEventGraphToText(
			AssetPath, GraphName, bIncludePositions, bStableIds));
}

FAnimBP2FPMCPMappingResult UAnimBP2FPToolset::GetAnimBlueprintSyncState(const FString& AssetPath)
{
	FAnimBP2FPMCPMappingResult Result;
	const FString PackagePath = AnimBP2FPMCP::NormalizePackagePath(AssetPath);
	const FBP2FPMappingEntry* Entry = FBP2FPMappingRegistry::Get().FindByBlueprint(PackagePath);
	if (!Entry)
	{
		Result.Error = FString::Printf(TEXT("No mapping found for AnimBlueprint: %s"), *PackagePath);
		return Result;
	}

	Result.bSuccess = true;
	Result.AssetPath = Entry->BlueprintPath;
	Result.DSLFilePath = Entry->DSLFilePath;
	Result.Category = Entry->CategoryTag;
	Result.State = AnimBP2FPMCP::MappingState(Entry->State);
	Result.bBlueprintExists = Entry->bBlueprintExists;
	Result.bDSLExists = Entry->bDSLFileExists;
	return Result;
}

FAnimBP2FPMCPBundleResult UAnimBP2FPToolset::ApplyAnimBundle(
	const TArray<FAnimBP2FPMCPBundleSource>& Sources,
	const FString& TargetRoot,
	const EAnimBP2FPMCPBundleMode Mode,
	const bool bCommitPersistent)
{
	FAnimBP2FPMCPBundleResult Result;
	if (Sources.IsEmpty())
	{
		Result.Error = TEXT("At least one bundle source is required");
		return Result;
	}

	TArray<FAnimLispBundleSource> BundleSources;
	BundleSources.Reserve(Sources.Num());
	TSet<FString> SourceNames;
	for (int32 Index = 0; Index < Sources.Num(); ++Index)
	{
		const FAnimBP2FPMCPBundleSource& Source = Sources[Index];
		const FString SourceName = Source.SourceFile.IsEmpty()
			? FString::Printf(TEXT("mcp-source-%d.animlang"), Index)
			: Source.SourceFile;
		if (!AnimBP2FPMCP::IsSafeSourceName(SourceName))
		{
			Result.Error = FString::Printf(
				TEXT("Invalid logical bundle source name: %s"), *SourceName);
			return Result;
		}
		if (SourceNames.Contains(SourceName))
		{
			Result.Error = FString::Printf(TEXT("Duplicate bundle source name: %s"), *SourceName);
			return Result;
		}
		SourceNames.Add(SourceName);
		BundleSources.Add({SourceName, Source.Source});
	}

	FAnimLispBundleImportOptions Options;
	Options.Mode = Mode == EAnimBP2FPMCPBundleMode::Strict
		? EAnimLispBundleImportMode::Strict
		: EAnimLispBundleImportMode::Legacy;
	Options.TargetRoot = TargetRoot;
	Options.bCommitPersistent = bCommitPersistent;

	const FAnimLispBundleImportResult ImportResult = FAnimBPImporter::ImportBundle(BundleSources, Options);
	Result.bSuccess = ImportResult.bSuccess;
	Result.bMutationStarted = ImportResult.bMutationStarted;
	Result.bPersisted = bCommitPersistent && ImportResult.bSuccess;
	for (const FAnimLispImportPlanEntry& Entry : ImportResult.Plan)
	{
		Result.ModulePlan.Add(Entry.ModuleId.ToString());
	}
	for (UObject* Asset : ImportResult.StagedAssets)
	{
		if (Asset)
		{
			Result.AssetPaths.Add(Asset->GetPathName());
		}
	}
	for (const FAnimLangDiagnostic& Diagnostic : ImportResult.Diagnostics.Items)
	{
		Result.Diagnostics.Add(AnimBP2FPMCP::ToDiagnostic(Diagnostic));
	}
	if (!Result.bSuccess)
	{
		Result.Error = ImportResult.Diagnostics.ToReport();
		if (Result.Error.IsEmpty())
		{
			Result.Error = TEXT("Anim/Rig bundle import failed");
		}
	}
	return Result;
}

FAnimBP2FPMCPChangeResult UAnimBP2FPToolset::UpdateAnimBlueprint(
	const FString& AssetPath,
	const FString& DSLText,
	const bool bSavePackage)
{
	return AnimBP2FPMCP::FromChangeResult(
		UAnimBP2FPPythonBridge::UpdateAnimBlueprintFromText(AssetPath, DSLText, bSavePackage),
		bSavePackage);
}

FAnimBP2FPMCPChangeResult UAnimBP2FPToolset::ReplaceEventGraph(
	const FString& AssetPath,
	const FString& GraphName,
	const FString& DSLText,
	const bool bCompile,
	const bool bSavePackage)
{
	return AnimBP2FPMCP::FromChangeResult(
		UAnimBP2FPPythonBridge::ImportEventGraphFromText(
			AssetPath, GraphName, DSLText, bCompile, bSavePackage),
		bSavePackage);
}

FAnimBP2FPMCPChangeResult UAnimBP2FPToolset::MergeEventGraph(
	const FString& AssetPath,
	const FString& GraphName,
	const FString& DSLText,
	const bool bCompile,
	const bool bSavePackage)
{
	return AnimBP2FPMCP::FromChangeResult(
		UAnimBP2FPPythonBridge::UpdateEventGraphFromText(
			AssetPath, GraphName, DSLText, bCompile, bSavePackage),
		bSavePackage);
}

#endif
