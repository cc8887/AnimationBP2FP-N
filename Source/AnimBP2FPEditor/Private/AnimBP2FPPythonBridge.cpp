// AnimBP2FPPythonBridge.cpp - Python-facing editor bridge for AnimBP2FP
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPPythonBridge.h"

#include "AnimBPExporter.h"
#include "AnimBPImporter.h"
#include "AnimNodeExporter.h"
#include "AnimLangRoundTrip.h"
#include "FBP2FPMappingRegistry.h"
#include "BlueprintLispConverter.h"
#include "Animation/AnimBlueprint.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"


namespace AnimBP2FPPythonBridge
{
	static FAnimBP2FPPythonResult MakeFailure(const FString& Message)
	{
		FAnimBP2FPPythonResult Result;
		Result.bSuccess = false;
		Result.Message = Message;
		return Result;
	}

	static FString NormalizeAnimBlueprintObjectPath(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			return Path;
		}

		if (Path.Contains(TEXT("'")))
		{
			return Path;
		}

		if (Path.Contains(TEXT(".")))
		{
			return Path;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		if (!AssetName.IsEmpty())
		{
			return Path + TEXT(".") + AssetName;
		}

		return Path;
	}

	static UAnimBlueprint* LoadAnimBlueprintByPath(const FString& AnimBlueprintPath, FString& OutResolvedPath, FString& OutError)
	{
		if (AnimBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("AnimBlueprintPath is empty");
			return nullptr;
		}

		OutResolvedPath = NormalizeAnimBlueprintObjectPath(AnimBlueprintPath);
		UAnimBlueprint* AnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *OutResolvedPath);
		if (!AnimBlueprint)
		{
			OutError = FString::Printf(TEXT("Failed to load AnimBlueprint: %s"), *AnimBlueprintPath);
		}
		return AnimBlueprint;
	}

	static bool ReadTextFile(const FString& FilePath, FString& OutText, FString& OutError)
	{
		if (FilePath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("FilePath is empty");
			return false;
		}

		if (!FFileHelper::LoadFileToString(OutText, *FilePath))
		{
			OutError = FString::Printf(TEXT("Failed to read file: %s"), *FilePath);
			return false;
		}

		return true;
	}

	static bool WriteTextFile(const FString& FilePath, const FString& Text, FString& OutError)
	{
		if (FilePath.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("Output file path is empty");
			return false;
		}

		const FString Directory = FPaths::GetPath(FilePath);
		if (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
		{
			IFileManager::Get().MakeDirectory(*Directory, true);
		}

		if (!FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write file: %s"), *FilePath);
			return false;
		}

		return true;
	}

	static bool SaveBlueprintPackage(UAnimBlueprint* AnimBlueprint, FString& OutError)
	{
		if (!AnimBlueprint)
		{
			OutError = TEXT("Blueprint is null");
			return false;
		}

		UPackage* Package = AnimBlueprint->GetPackage();
		if (!Package)
		{
			OutError = TEXT("Blueprint package is null");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			OutError = FString::Printf(TEXT("Failed to save package for %s"), *AnimBlueprint->GetPathName());
			return false;
		}

		return true;
	}

	static FString BuildUpdateMessage(const FAnimBPImporter::FUpdateResult& UpdateResult)
	{
		return FString::Printf(
			TEXT("Update %s (%s): %d changes (%d property, %d structural)"),
			UpdateResult.bSuccess ? TEXT("succeeded") : TEXT("failed"),
			UpdateResult.bUsedIncrementalPatch ? TEXT("incremental") : TEXT("full rebuild"),
			UpdateResult.NumChanges,
			UpdateResult.NumPropertyChanges,
			UpdateResult.NumStructuralChanges);
	}

	static FAnimBP2FPPythonResult FromBlueprintLispResult(
		const FBlueprintLispResult& InResult,
		const FString& AssetPath,
		const FString& SuccessMessage)
	{
		FAnimBP2FPPythonResult Result;
		Result.bSuccess = InResult.bSuccess;
		Result.AssetPath = AssetPath;
		Result.DSLText = InResult.LispCode;
		Result.Warnings = InResult.Warnings;
		if (!InResult.Error.IsEmpty())
		{
			Result.Warnings.Insert(InResult.Error, 0);
		}
		Result.Message = InResult.bSuccess
			? SuccessMessage
			: (InResult.Error.IsEmpty() ? TEXT("Operation failed") : InResult.Error);
		return Result;
	}

	static FAnimBP2FPPythonResult ImportEventGraphInternal(
		UAnimBlueprint* AnimBlueprint,
		const FString& ResolvedPath,
		const FString& GraphName,
		const FString& DSLText,
		bool bCompile,
		bool bSavePackage,
		const FString& SuccessMessage)
	{
		if (!AnimBlueprint)
		{
			return MakeFailure(TEXT("AnimBlueprint is null"));
		}

		FBlueprintLispConverter::FImportOptions Options;
		Options.ImportMode = FBlueprintLispConverter::EImportMode::ReplaceGraph;
		Options.bCompile = bCompile;
		Options.bAutoLayout = true;
		Options.bFailOnUnsupportedForm = true;

		FBlueprintLispResult LispResult = FBlueprintLispConverter::Import(AnimBlueprint, GraphName, DSLText, Options);
		FAnimBP2FPPythonResult Result = FromBlueprintLispResult(LispResult, ResolvedPath, SuccessMessage);
		Result.DSLText = DSLText;

		if (!Result.bSuccess)
		{
			return Result;
		}

		if (bSavePackage)
		{
			FString SaveError;
			Result.bSavedPackage = SaveBlueprintPackage(AnimBlueprint, SaveError);
			if (!Result.bSavedPackage)
			{
				Result.Warnings.Add(SaveError);
				Result.Message += TEXT(" (package save failed)");
			}
		}

		return Result;
	}
}


FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ExportAnimBlueprintToText(const FString& AnimBlueprintPath)
{
	FString ResolvedPath;
	FString Error;
	UAnimBlueprint* AnimBlueprint = AnimBP2FPPythonBridge::LoadAnimBlueprintByPath(AnimBlueprintPath, ResolvedPath, Error);
	if (!AnimBlueprint)
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	FAnimBP2FPPythonResult Result;
	Result.AssetPath = ResolvedPath;
	Result.DSLText = FAnimBPExporter::Export(AnimBlueprint);
	Result.bSuccess = !Result.DSLText.IsEmpty() && !Result.DSLText.StartsWith(TEXT("; Error:"));
	Result.Message = Result.bSuccess
		? FString::Printf(TEXT("Exported AnimBlueprint to DSL: %s"), *ResolvedPath)
		: Result.DSLText;
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ExportAnimBlueprintToFile(const FString& AnimBlueprintPath, const FString& OutputFilePath)
{
	FAnimBP2FPPythonResult Result = ExportAnimBlueprintToText(AnimBlueprintPath);
	if (!Result.bSuccess)
	{
		return Result;
	}

	FString Error;
	if (!AnimBP2FPPythonBridge::WriteTextFile(OutputFilePath, Result.DSLText, Error))
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	Result.FilePath = OutputFilePath;
	Result.Message = FString::Printf(TEXT("Exported AnimBlueprint to file: %s"), *OutputFilePath);
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ImportAnimBlueprintFromText(const FString& DSLText, const FString& DestinationFolder, bool bSavePackage)
{
	if (DSLText.TrimStartAndEnd().IsEmpty())
	{
		return AnimBP2FPPythonBridge::MakeFailure(TEXT("DSLText is empty"));
	}

	if (DestinationFolder.TrimStartAndEnd().IsEmpty())
	{
		return AnimBP2FPPythonBridge::MakeFailure(TEXT("DestinationFolder is empty"));
	}

	FString Error;
	UAnimBlueprint* AnimBlueprint = FAnimBPImporter::Import(DSLText, DestinationFolder, &Error);
	if (!AnimBlueprint)
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error.IsEmpty() ? TEXT("Import failed") : Error);
	}

	FAnimBP2FPPythonResult Result;
	Result.bSuccess = true;
	Result.AssetPath = AnimBlueprint->GetPathName();
	Result.DSLText = DSLText;
	Result.Message = FString::Printf(TEXT("Imported AnimBlueprint: %s"), *Result.AssetPath);

	if (bSavePackage)
	{
		FString SaveError;
		Result.bSavedPackage = AnimBP2FPPythonBridge::SaveBlueprintPackage(AnimBlueprint, SaveError);
		if (!Result.bSavedPackage)
		{
			Result.Warnings.Add(SaveError);
			Result.Message += TEXT(" (package save failed)");
		}
	}

	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ImportAnimBlueprintFromFile(const FString& InputFilePath, const FString& DestinationFolder, bool bSavePackage)
{
	FString DSLText;
	FString Error;
	if (!AnimBP2FPPythonBridge::ReadTextFile(InputFilePath, DSLText, Error))
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	FAnimBP2FPPythonResult Result = ImportAnimBlueprintFromText(DSLText, DestinationFolder, bSavePackage);
	Result.FilePath = InputFilePath;
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::UpdateAnimBlueprintFromText(const FString& AnimBlueprintPath, const FString& DSLText, bool bSavePackage)
{
	if (DSLText.TrimStartAndEnd().IsEmpty())
	{
		return AnimBP2FPPythonBridge::MakeFailure(TEXT("DSLText is empty"));
	}

	FString ResolvedPath;
	FString Error;
	UAnimBlueprint* AnimBlueprint = AnimBP2FPPythonBridge::LoadAnimBlueprintByPath(AnimBlueprintPath, ResolvedPath, Error);
	if (!AnimBlueprint)
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	const FAnimBPImporter::FUpdateResult UpdateResult = FAnimBPImporter::UpdateBlueprintDetailed(AnimBlueprint, DSLText);

	FAnimBP2FPPythonResult Result;
	Result.bSuccess = UpdateResult.bSuccess;
	Result.AssetPath = ResolvedPath;
	Result.DSLText = DSLText;
	Result.bUsedIncrementalPatch = UpdateResult.bUsedIncrementalPatch;
	Result.NumChanges = UpdateResult.NumChanges;
	Result.NumPropertyChanges = UpdateResult.NumPropertyChanges;
	Result.NumStructuralChanges = UpdateResult.NumStructuralChanges;
	Result.AppliedOps = UpdateResult.AppliedOps;
	Result.Warnings = UpdateResult.Warnings;
	Result.Message = AnimBP2FPPythonBridge::BuildUpdateMessage(UpdateResult);

	if (!Result.bSuccess)
	{
		return Result;
	}

	if (bSavePackage)
	{
		FString SaveError;
		Result.bSavedPackage = AnimBP2FPPythonBridge::SaveBlueprintPackage(AnimBlueprint, SaveError);
		if (!Result.bSavedPackage)
		{
			Result.Warnings.Add(SaveError);
			Result.Message += TEXT(" (package save failed)");
		}
	}

	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::UpdateAnimBlueprintFromFile(const FString& AnimBlueprintPath, const FString& InputFilePath, bool bSavePackage)
{
	FString DSLText;
	FString Error;
	if (!AnimBP2FPPythonBridge::ReadTextFile(InputFilePath, DSLText, Error))
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	FAnimBP2FPPythonResult Result = UpdateAnimBlueprintFromText(AnimBlueprintPath, DSLText, bSavePackage);
	Result.FilePath = InputFilePath;
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ExportEventGraphToText(
	const FString& AnimBlueprintPath,
	const FString& GraphName,
	bool bIncludePositions,
	bool bStableIds)
{
	FString ResolvedPath;
	FString Error;
	UAnimBlueprint* AnimBlueprint = AnimBP2FPPythonBridge::LoadAnimBlueprintByPath(AnimBlueprintPath, ResolvedPath, Error);
	if (!AnimBlueprint)
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	FAnimBPExporter::FEventGraphExportOptions Options;
	Options.GraphName = GraphName;
	Options.bPrettyPrint = true;
	Options.bIncludePositions = bIncludePositions;
	Options.bStableIds = bStableIds;

	FAnimBP2FPPythonResult Result;
	Result.AssetPath = ResolvedPath;
	Result.bSuccess = FAnimBPExporter::ExportEventGraph(AnimBlueprint, Options, Result.DSLText, Error);
	Result.Message = Result.bSuccess
		? FString::Printf(TEXT("Exported graph '%s' to BlueprintLisp: %s"), *GraphName, *ResolvedPath)
		: Error;
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ExportEventGraphToFile(
	const FString& AnimBlueprintPath,
	const FString& OutputFilePath,
	const FString& GraphName,
	bool bIncludePositions,
	bool bStableIds)
{
	FAnimBP2FPPythonResult Result = ExportEventGraphToText(AnimBlueprintPath, GraphName, bIncludePositions, bStableIds);
	if (!Result.bSuccess)
	{
		return Result;
	}

	FString Error;
	if (!AnimBP2FPPythonBridge::WriteTextFile(OutputFilePath, Result.DSLText, Error))
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	Result.FilePath = OutputFilePath;
	Result.Message = FString::Printf(TEXT("Exported graph '%s' to file: %s"), *GraphName, *OutputFilePath);
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ImportEventGraphFromText(
	const FString& AnimBlueprintPath,
	const FString& GraphName,
	const FString& DSLText,
	bool bCompile,
	bool bSavePackage)
{
	if (DSLText.TrimStartAndEnd().IsEmpty())
	{
		return AnimBP2FPPythonBridge::MakeFailure(TEXT("DSLText is empty"));
	}

	FString ResolvedPath;
	FString Error;
	UAnimBlueprint* AnimBlueprint = AnimBP2FPPythonBridge::LoadAnimBlueprintByPath(AnimBlueprintPath, ResolvedPath, Error);
	if (!AnimBlueprint)
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	return AnimBP2FPPythonBridge::ImportEventGraphInternal(
		AnimBlueprint,
		ResolvedPath,
		GraphName,
		DSLText,
		bCompile,
		bSavePackage,
		FString::Printf(TEXT("Imported graph '%s' into %s"), *GraphName, *ResolvedPath));
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ImportEventGraphFromFile(
	const FString& AnimBlueprintPath,
	const FString& GraphName,
	const FString& InputFilePath,
	bool bCompile,
	bool bSavePackage)
{
	FString DSLText;
	FString Error;
	if (!AnimBP2FPPythonBridge::ReadTextFile(InputFilePath, DSLText, Error))
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	FAnimBP2FPPythonResult Result = ImportEventGraphFromText(
		AnimBlueprintPath,
		GraphName,
		DSLText,
		bCompile,
		bSavePackage);
	Result.FilePath = InputFilePath;
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::UpdateEventGraphFromText(
	const FString& AnimBlueprintPath,
	const FString& GraphName,
	const FString& DSLText,
	bool bCompile,
	bool bSavePackage)
{
	FAnimBP2FPPythonResult Result = ImportEventGraphFromText(
		AnimBlueprintPath,
		GraphName,
		DSLText,
		bCompile,
		bSavePackage);

	if (Result.bSuccess)
	{
		Result.Warnings.Insert(
			TEXT("BlueprintLisp semantic Update is not implemented yet; UpdateEventGraph currently uses ReplaceGraph import semantics."),
			0);
		Result.Message = FString::Printf(TEXT("Updated graph '%s' in %s (ReplaceGraph fallback)"), *GraphName, *Result.AssetPath);
	}

	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::UpdateEventGraphFromFile(
	const FString& AnimBlueprintPath,
	const FString& GraphName,
	const FString& InputFilePath,
	bool bCompile,
	bool bSavePackage)
{
	FString DSLText;
	FString Error;
	if (!AnimBP2FPPythonBridge::ReadTextFile(InputFilePath, DSLText, Error))
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	FAnimBP2FPPythonResult Result = UpdateEventGraphFromText(
		AnimBlueprintPath,
		GraphName,
		DSLText,
		bCompile,
		bSavePackage);
	Result.FilePath = InputFilePath;
	return Result;
}

// ========== Mapping Registry ==========


FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::GetMappingTable()
{
	FBP2FPMappingRegistry& Registry = FBP2FPMappingRegistry::Get();

	FAnimBP2FPPythonResult Result;
	Result.bSuccess = true;

	TArray<FString> JsonEntries;
	for (const FBP2FPMappingEntry& Entry : Registry.GetAllEntries())
	{
		FString StateStr;
		switch (Entry.State)
		{
		case EBP2FPSyncState::Synced:    StateStr = TEXT("Synced");    break;
		case EBP2FPSyncState::BPOnly:    StateStr = TEXT("BPOnly");    break;
		case EBP2FPSyncState::DSLOnly:   StateStr = TEXT("DSLOnly");   break;
		case EBP2FPSyncState::OutOfSync: StateStr = TEXT("OutOfSync"); break;
		}

		JsonEntries.Add(FString::Printf(
			TEXT("{\"blueprint_path\":\"%s\",\"dsl_file_path\":\"%s\",\"category\":\"%s\",\"state\":\"%s\",\"has_blueprint\":%s,\"has_dsl\":%s}"),
			*Entry.BlueprintPath,
			*Entry.DSLFilePath,
			*Entry.CategoryTag,
			*StateStr,
			Entry.bBlueprintExists ? TEXT("true") : TEXT("false"),
			Entry.bDSLFileExists ? TEXT("true") : TEXT("false")
		));
	}

	Result.DSLText = TEXT("[") + FString::Join(JsonEntries, TEXT(",")) + TEXT("]");
	Result.Message = FString::Printf(TEXT("Mapping table: %d entries"), Registry.Num());
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::FindMappingByBlueprint(const FString& AnimBlueprintPath)
{
	FBP2FPMappingRegistry& Registry = FBP2FPMappingRegistry::Get();
	const FBP2FPMappingEntry* Entry = Registry.FindByBlueprint(AnimBlueprintPath);

	if (!Entry)
	{
		FAnimBP2FPPythonResult Result;
		Result.bSuccess = false;
		Result.Message = FString::Printf(TEXT("No mapping found for: %s"), *AnimBlueprintPath);
		return Result;
	}

	FString StateStr;
	switch (Entry->State)
	{
	case EBP2FPSyncState::Synced:    StateStr = TEXT("Synced");    break;
	case EBP2FPSyncState::BPOnly:    StateStr = TEXT("BPOnly");    break;
	case EBP2FPSyncState::DSLOnly:   StateStr = TEXT("DSLOnly");   break;
	case EBP2FPSyncState::OutOfSync: StateStr = TEXT("OutOfSync"); break;
	}

	FAnimBP2FPPythonResult Result;
	Result.bSuccess = true;
	Result.AssetPath = Entry->BlueprintPath;
	Result.FilePath = Entry->DSLFilePath;
	Result.Message = FString::Printf(TEXT("State: %s | Blueprint: %s | DSL: %s"),
		*StateStr, Entry->bBlueprintExists ? TEXT("yes") : TEXT("no"), Entry->bDSLFileExists ? TEXT("yes") : TEXT("no"));
	return Result;
}

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::AnimBlueprintPathToDSLPath(const FString& AnimBlueprintPath)
{
	FString DSLPath = FBP2FPMappingRegistry::BlueprintToDSLPath(AnimBlueprintPath, TEXT("AnimBP"), TEXT(".animlang"));

	if (DSLPath.IsEmpty())
	{
		FAnimBP2FPPythonResult Result;
		Result.bSuccess = false;
		Result.Message = FString::Printf(TEXT("Invalid AnimBlueprint path (engine/system content not exportable): %s"), *AnimBlueprintPath);
		return Result;
	}

	FAnimBP2FPPythonResult Result;
	Result.bSuccess = true;
	Result.AssetPath = AnimBlueprintPath;
	Result.FilePath = DSLPath;
	Result.Message = FString::Printf(TEXT("%s -> %s"), *AnimBlueprintPath, *DSLPath);
	return Result;
}

// ========== Validation ==========

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ValidateAnimBlueprintRoundTrip(const FString& AnimBlueprintPath)
{
	FString ResolvedPath;
	FString Error;
	UAnimBlueprint* AnimBlueprint = AnimBP2FPPythonBridge::LoadAnimBlueprintByPath(AnimBlueprintPath, ResolvedPath, Error);
	if (!AnimBlueprint)
	{
		return AnimBP2FPPythonBridge::MakeFailure(Error);
	}

	// Export to DSL
	FString DSLText = FAnimBPExporter::Export(AnimBlueprint);
	if (DSLText.IsEmpty() || DSLText.StartsWith(TEXT("; Error:")))
	{
		FAnimBP2FPPythonResult Result;
		Result.bSuccess = false;
		Result.AssetPath = ResolvedPath;
		Result.Message = FString::Printf(TEXT("Export failed for: %s"), *ResolvedPath);
		if (!DSLText.IsEmpty())
		{
			Result.Warnings.Add(DSLText);
		}
		return Result;
	}

	// Run round-trip validation on the DSL text
	FRoundTripResult RoundTrip = FAnimLangRoundTrip::TestString(DSLText, AnimBlueprint->GetName());

	FAnimBP2FPPythonResult Result;
	Result.AssetPath = ResolvedPath;
	Result.bSuccess = RoundTrip.bSuccess;
	Result.DSLText = DSLText;
	Result.Warnings = RoundTrip.Differences;
	if (RoundTrip.ParseErrors.Num() > 0)
	{
		Result.Warnings.Append(RoundTrip.ParseErrors);
	}
	Result.Message = FString::Printf(
		TEXT("RoundTrip %s: %.1f%% similarity (%d/%d lines differ)"),
		RoundTrip.bSuccess ? TEXT("PASS") : TEXT("FAIL"),
		RoundTrip.SimilarityPercent,
		RoundTrip.OriginalLines - RoundTrip.RoundTrippedLines,
		RoundTrip.OriginalLines);
	return Result;
}

// ========== Stub Export ==========

FAnimBP2FPPythonResult UAnimBP2FPPythonBridge::ExportStub(const FString& OutputFilePath)
{
#if WITH_EDITOR
	FString StubPath = OutputFilePath;
	if (StubPath.TrimStartAndEnd().IsEmpty())
	{
		StubPath = FPaths::ProjectDir() / TEXT("Saved") / TEXT("BP2DSL") / TEXT("AnimBP") / TEXT("animlang-nodes-generated.rkt");
	}
	FPaths::NormalizeFilename(StubPath);

	bool bOk = FAnimNodeExporter::ExportAllNodes(StubPath);

	FAnimBP2FPPythonResult Result;
	Result.bSuccess = bOk;
	Result.FilePath = StubPath;
	Result.Message = bOk
		? FString::Printf(TEXT("Animation node stub exported to: %s"), *StubPath)
		: TEXT("Failed to export animation node stub");
	return Result;
#else
	FAnimBP2FPPythonResult Result;
	Result.bSuccess = false;
	Result.Message = TEXT("Stub export is only available in editor builds");
	return Result;
#endif
}
