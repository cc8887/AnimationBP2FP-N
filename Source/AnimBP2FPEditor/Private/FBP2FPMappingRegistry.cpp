// FBP2FPMappingRegistry.cpp - In-memory Blueprint <-> DSL Lookup Table
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "FBP2FPMappingRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Animation/AnimBlueprint.h"
#include "Interfaces/IPluginManager.h"

// ========== Singleton ==========

FBP2FPMappingRegistry& FBP2FPMappingRegistry::Get()
{
	static FBP2FPMappingRegistry Instance;
	return Instance;
}

// ========== Lifecycle ==========

void FBP2FPMappingRegistry::Initialize()
{
	Reset();

	UE_LOG(LogTemp, Log, TEXT("BP2FPMappingRegistry: Initializing..."));

	// Phase 1: Scan blueprints
	ScanBlueprints();

	// Phase 2: Scan DSL files
	ScanDSLFiles(TEXT("AnimBP"));
	ScanDSLFiles(TEXT("MatBP"));

	// Phase 3: Reconcile
	Reconcile();

	const int32 SyncedCount = Entries.FilterByPredicate(
		[](const FBP2FPMappingEntry& E) { return E.State == EBP2FPSyncState::Synced; }).Num();
	const int32 BPOnlyCount = Entries.FilterByPredicate(
		[](const FBP2FPMappingEntry& E) { return E.State == EBP2FPSyncState::BPOnly; }).Num();
	const int32 DSLOnlyCount = Entries.FilterByPredicate(
		[](const FBP2FPMappingEntry& E) { return E.State == EBP2FPSyncState::DSLOnly; }).Num();
	const int32 OutOfSyncCount = Entries.FilterByPredicate(
		[](const FBP2FPMappingEntry& E) { return E.State == EBP2FPSyncState::OutOfSync; }).Num();
	UE_LOG(LogTemp, Log, TEXT("BP2FPMappingRegistry: Initialized with %d entries (%d synced, %d BP-only, %d DSL-only, %d out-of-sync)"),
		Entries.Num(), SyncedCount, BPOnlyCount, DSLOnlyCount, OutOfSyncCount);
}

void FBP2FPMappingRegistry::Reset()
{
	Entries.Empty();
	BlueprintToIndex.Empty();
	DSLFileToIndex.Empty();
}

// ========== Queries ==========

const FBP2FPMappingEntry* FBP2FPMappingRegistry::FindByBlueprint(const FString& BlueprintPath) const
{
	const int32* IndexPtr = BlueprintToIndex.Find(BlueprintPath);
	if (!IndexPtr) return nullptr;
	if (!Entries.IsValidIndex(*IndexPtr)) return nullptr;
	return &Entries[*IndexPtr];
}

const FBP2FPMappingEntry* FBP2FPMappingRegistry::FindByDSLFile(const FString& DSLFilePath) const
{
	const int32* IndexPtr = DSLFileToIndex.Find(DSLFilePath);
	if (!IndexPtr) return nullptr;
	if (!Entries.IsValidIndex(*IndexPtr)) return nullptr;
	return &Entries[*IndexPtr];
}

// ========== Path Conversion (Internal Helpers) ==========

namespace
{
	// Strip the content root mount point prefix from a UE package path.
	// e.g., /Game/Characters/ALS/ALS_Npc -> Characters/ALS/ALS_Npc
	//        /MyPlugin/Characters/ALS/ALS_Npc -> Characters/ALS/ALS_Npc
	// Returns empty string for system mount points (/Engine/, /Script/, etc.).
	FString StripContentRootPrefix(const FString& PackagePath)
	{
		if (PackagePath.IsEmpty() || !PackagePath.StartsWith(TEXT("/")))
		{
			return FString();
		}

		// Find the second '/' which marks the end of the mount point
		int32 SecondSlash = PackagePath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
		if (SecondSlash <= 0)
		{
			return FString();
		}

		// Extract mount point name (between first and second slash)
		FString MountPoint = PackagePath.Mid(1, SecondSlash - 1);

		// Skip system/engine mount points that should not be exported
		if (MountPoint == TEXT("Engine") ||
			MountPoint == TEXT("Script") ||
			MountPoint == TEXT("Temp") ||
			MountPoint == TEXT("Transient"))
		{
			return FString();
		}

		// Return everything after the mount point
		return PackagePath.RightChop(SecondSlash + 1);
	}
}

// ========== Path Conversion (Public) ==========

bool FBP2FPMappingRegistry::IsExportablePackage(const FString& PackagePath)
{
	return !StripContentRootPrefix(PackagePath).IsEmpty();
}

FString FBP2FPMappingRegistry::BlueprintToDSLPath(
	const FString& BlueprintPath,
	const FString& CategoryTag,
	const FString& Extension)
{
	if (BlueprintPath.IsEmpty())
	{
		return FString();
	}

	// Dynamically strip any content root prefix (e.g., /Game/, /MyPlugin/)
	// Returns empty for engine/system paths
	FString RelativePath = StripContentRootPrefix(BlueprintPath);
	if (RelativePath.IsEmpty())
	{
		return FString();
	}

	// {ProjectDir}/Saved/BP2DSL/{Category}/Path/AssetName.ext
	FString DSLPath = FPaths::ProjectDir() /
		TEXT("Saved") / TEXT("BP2DSL") / CategoryTag / RelativePath;

	// Change extension
	FString BaseName = FPaths::GetBaseFilename(DSLPath);
	FString Dir = FPaths::GetPath(DSLPath);
	return Dir / (BaseName + Extension);
}

FString FBP2FPMappingRegistry::DSLToBlueprintPath(
	const FString& DSLFilePath,
	const FString& CategoryTag)
{
	// Expected: {ProjectDir}/Saved/BP2DSL/{Category}/Path/AssetName.ext
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString ExpectedPrefix = ProjectDir / TEXT("Saved") / TEXT("BP2DSL") / CategoryTag / TEXT("");

	if (!DSLFilePath.StartsWith(ExpectedPrefix))
	{
		return FString();
	}

	// Strip prefix: Path/AssetName.ext
	FString RelativePath = DSLFilePath.RightChop(ExpectedPrefix.Len());
	FString AssetName = FPaths::GetBaseFilename(RelativePath);

	// Phase 1: Try to resolve via AssetRegistry (supports any mount point)
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AllBPs;
#if ENGINE_MAJOR_VERSION < 5
	AssetRegistry.GetAssetsByClass(UAnimBlueprint::StaticClass()->GetFName(), AllBPs);
#else
	AssetRegistry.GetAssetsByClass(UAnimBlueprint::StaticClass()->GetClassPathName(), AllBPs);
#endif

	TArray<FString> Candidates;
	for (const FAssetData& Asset : AllBPs)
	{
		if (Asset.AssetName.ToString() == AssetName && IsExportablePackage(Asset.PackageName.ToString()))
		{
			Candidates.Add(Asset.PackageName.ToString());
		}
	}

	if (Candidates.Num() == 1)
	{
		return Candidates[0];
	}
	else if (Candidates.Num() > 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("BP2FPMappingRegistry: DSLToBlueprintPath: ambiguous asset name '%s' (%d candidates), falling back to /Game/"),
			*AssetName, Candidates.Num());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BP2FPMappingRegistry: DSLToBlueprintPath: no matching AnimBlueprint for '%s', falling back to /Game/"),
			*AssetName);
	}

	// Phase 2: Fallback to /Game/ convention
	return TEXT("/Game/") + FPaths::GetPath(RelativePath) / AssetName;
}

// ========== Mutation ==========

FBP2FPMappingEntry& FBP2FPMappingRegistry::GetOrCreateEntry(
	const FString& BlueprintPath,
	const FString& CategoryTag)
{
	const int32* IndexPtr = BlueprintToIndex.Find(BlueprintPath);
	if (IndexPtr && Entries.IsValidIndex(*IndexPtr))
	{
		return Entries[*IndexPtr];
	}

	// Create new entry
	FBP2FPMappingEntry NewEntry(BlueprintPath, CategoryTag);
	NewEntry.DSLFilePath = BlueprintToDSLPath(BlueprintPath, CategoryTag);

	int32 NewIndex = Entries.Add(MoveTemp(NewEntry));
	BlueprintToIndex.Add(BlueprintPath, NewIndex);
	if (!NewEntry.DSLFilePath.IsEmpty())
	{
		DSLFileToIndex.Add(NewEntry.DSLFilePath, NewIndex);
	}

	return Entries[NewIndex];
}

void FBP2FPMappingRegistry::MarkExported(const FString& BlueprintPath, const FString& DSLContent)
{
	const int32* IndexPtr = BlueprintToIndex.Find(BlueprintPath);
	if (!IndexPtr || !Entries.IsValidIndex(*IndexPtr)) return;

	FBP2FPMappingEntry& Entry = Entries[*IndexPtr];
	Entry.bBlueprintExists = true;
	Entry.bDSLFileExists = true;
	Entry.State = EBP2FPSyncState::Synced;
	Entry.LastExportTime = FDateTime::Now();

	// Update hash: use string hash as lightweight content fingerprint
	Entry.DSLContentHash = LexToString(GetTypeHash(DSLContent));

	// Ensure DSL file index is up to date
	if (!Entry.DSLFilePath.IsEmpty())
	{
		DSLFileToIndex.Add(Entry.DSLFilePath, *IndexPtr);
	}
}

void FBP2FPMappingRegistry::MarkImported(const FString& DSLFilePath)
{
	const int32* IndexPtr = DSLFileToIndex.Find(DSLFilePath);
	if (!IndexPtr || !Entries.IsValidIndex(*IndexPtr)) return;

	FBP2FPMappingEntry& Entry = Entries[*IndexPtr];
	Entry.bBlueprintExists = true;
	Entry.bDSLFileExists = true;
	Entry.State = EBP2FPSyncState::Synced;
}

// ========== Internal Scanning ==========

void FBP2FPMappingRegistry::ScanBlueprints()
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Scan Animation Blueprints
	TArray<FAssetData> AnimBPAssets;
#if ENGINE_MAJOR_VERSION < 5
	AssetRegistry.GetAssetsByClass(UAnimBlueprint::StaticClass()->GetFName(), AnimBPAssets);
#else
	AssetRegistry.GetAssetsByClass(UAnimBlueprint::StaticClass()->GetClassPathName(), AnimBPAssets);
#endif

	for (const FAssetData& AssetData : AnimBPAssets)
	{
		FString PackagePath = AssetData.PackageName.ToString();
		FBP2FPMappingEntry Entry(PackagePath, TEXT("AnimBP"));
		Entry.DSLFilePath = BlueprintToDSLPath(PackagePath, TEXT("AnimBP"), TEXT(".animlang"));
		Entry.bBlueprintExists = true;
		Entry.bDSLFileExists = FPaths::FileExists(Entry.DSLFilePath);
		Entry.State = Entry.bDSLFileExists ? EBP2FPSyncState::OutOfSync : EBP2FPSyncState::BPOnly;

		int32 Idx = Entries.Add(MoveTemp(Entry));
		BlueprintToIndex.Add(PackagePath, Idx);
		if (!Entries[Idx].DSLFilePath.IsEmpty())
		{
			DSLFileToIndex.Add(Entries[Idx].DSLFilePath, Idx);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("BP2FPMappingRegistry: Scanned %d Animation Blueprints"), AnimBPAssets.Num());
}

void FBP2FPMappingRegistry::ScanDSLFiles(const FString& CategoryTag)
{
	FString ScanDir = FPaths::ProjectDir() / TEXT("Saved") / TEXT("BP2DSL") / CategoryTag;

	if (!IFileManager::Get().DirectoryExists(*ScanDir))
	{
		return;
	}

	TArray<FString> FoundFiles;
	IFileManager::Get().FindFilesRecursive(FoundFiles, *ScanDir, TEXT("*.bplisp"), true, false);

	// Also scan .animlang files for AnimBP category
	if (CategoryTag == TEXT("AnimBP"))
	{
		TArray<FString> AnimLangFiles;
		IFileManager::Get().FindFilesRecursive(AnimLangFiles, *ScanDir, TEXT("*.animlang"), true, false);
		FoundFiles.Append(AnimLangFiles);
	}

	// Build asset name -> package path lookup from AssetRegistry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TMap<FString, TArray<FString>> BPNameToPaths;
	TArray<FAssetData> AllBPs;
#if ENGINE_MAJOR_VERSION < 5
	AssetRegistry.GetAssetsByClass(UAnimBlueprint::StaticClass()->GetFName(), AllBPs);
#else
	AssetRegistry.GetAssetsByClass(UAnimBlueprint::StaticClass()->GetClassPathName(), AllBPs);
#endif
	for (const FAssetData& Asset : AllBPs)
	{
		FString PkgPath = Asset.PackageName.ToString();
		if (IsExportablePackage(PkgPath))
		{
			BPNameToPaths.FindOrAdd(Asset.AssetName.ToString()).Add(PkgPath);
		}
	}

	for (const FString& FilePath : FoundFiles)
	{
		FString AbsPath = FPaths::ConvertRelativePathToFull(FilePath);

		// Skip if already mapped (from BP scan)
		if (DSLFileToIndex.Contains(AbsPath))
		{
			continue;
		}

		// Try to resolve blueprint path by looking up asset name in registry
		FString AssetName = FPaths::GetBaseFilename(AbsPath);
		FString BPPath;
		bool bAmbiguous = false;

		if (const TArray<FString>* Paths = BPNameToPaths.Find(AssetName))
		{
			if (Paths->Num() == 1)
			{
				BPPath = (*Paths)[0];
			}
			else if (Paths->Num() > 1)
			{
				bAmbiguous = true;
			}
		}

		// Fallback: guess path using /Game/ convention
		bool bGuessedPath = false;
		if (BPPath.IsEmpty() && !bAmbiguous)
		{
			BPPath = DSLToBlueprintPath(AbsPath, CategoryTag);
			bGuessedPath = !BPPath.IsEmpty();
		}

		FBP2FPMappingEntry Entry;
		Entry.DSLFilePath = AbsPath;
		Entry.CategoryTag = CategoryTag;
		Entry.bDSLFileExists = true;

		if (!BPPath.IsEmpty())
		{
			Entry.BlueprintPath = BPPath;
			Entry.bBlueprintExists = FPackageName::DoesPackageExist(BPPath);
			Entry.State = EBP2FPSyncState::DSLOnly;

			if (bAmbiguous)
			{
				UE_LOG(LogTemp, Warning, TEXT("BP2FPMappingRegistry: Ambiguous blueprint name '%s' (%d matches), DSL: %s"),
					*AssetName, BPNameToPaths[AssetName].Num(), *FPaths::GetCleanFilename(AbsPath));
			}
			else if (bGuessedPath && !Entry.bBlueprintExists)
			{
				UE_LOG(LogTemp, Warning, TEXT("BP2FPMappingRegistry: DSL file '%s' has no matching blueprint (guessed: %s)"),
					*FPaths::GetCleanFilename(AbsPath), *BPPath);
			}
		}
		else
		{
			Entry.State = EBP2FPSyncState::DSLOnly;

			if (bAmbiguous)
			{
				UE_LOG(LogTemp, Warning, TEXT("BP2FPMappingRegistry: DSL file '%s' - ambiguous blueprint name '%s' (%d matches)"),
					*FPaths::GetCleanFilename(AbsPath), *AssetName, BPNameToPaths[AssetName].Num());
			}
		}

		int32 Idx = Entries.Add(MoveTemp(Entry));
		DSLFileToIndex.Add(AbsPath, Idx);
		if (!Entries[Idx].BlueprintPath.IsEmpty())
		{
			BlueprintToIndex.Add(Entries[Idx].BlueprintPath, Idx);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("BP2FPMappingRegistry: Scanned %d DSL files in %s"), FoundFiles.Num(), *CategoryTag);
}

void FBP2FPMappingRegistry::Reconcile()
{
	// For entries with both BP and DSL, mark as Synced (content hash check deferred)
	for (FBP2FPMappingEntry& Entry : Entries)
	{
		if (Entry.bBlueprintExists && Entry.bDSLFileExists && Entry.State == EBP2FPSyncState::BPOnly)
		{
			// BP scan found DSL file exists too - mark as potentially synced
			Entry.State = EBP2FPSyncState::OutOfSync; // Will be confirmed on next actual export
		}
	}
}
