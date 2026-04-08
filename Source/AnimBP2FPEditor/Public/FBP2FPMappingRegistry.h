// FBP2FPMappingRegistry.h - In-memory Blueprint <-> DSL Lookup Table
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FBP2FPMapping.h"

/**
 * Singleton registry maintaining an in-memory lookup table for
 * Blueprint <-> DSL bidirectional path mapping.
 *
 * Built at engine startup by scanning AssetRegistry (blueprints)
 * and FileManager (DSL files), then reconciled.
 *
 * Thread safety: NOT thread-safe. All access must be on the game thread.
 */
class ANIMBP2FPEDITOR_API FBP2FPMappingRegistry
{
public:
	/** Get the singleton instance */
	static FBP2FPMappingRegistry& Get();

	// ========== Lifecycle ==========

	/**
	 * Initialize the registry.
	 * Scans all blueprints via AssetRegistry, scans DSL output directory,
	 * then reconciles to establish initial mapping states.
	 */
	void Initialize();

	/** Clear all entries */
	void Reset();

	// ========== Queries ==========

	/** Find a mapping entry by Blueprint package path. Returns nullptr if not found. */
	const FBP2FPMappingEntry* FindByBlueprint(const FString& BlueprintPath) const;

	/** Find a mapping entry by DSL absolute file path. Returns nullptr if not found. */
	const FBP2FPMappingEntry* FindByDSLFile(const FString& DSLFilePath) const;

	/** Get all entries */
	const TArray<FBP2FPMappingEntry>& GetAllEntries() const { return Entries; }

	/** Number of entries */
	int32 Num() const { return Entries.Num(); }

	// ========== Path Conversion ==========

	/**
	 * Check if a package path belongs to an exportable content root.
	 * Returns true for /Game/, plugin mount points, etc.
	 * Returns false for /Engine/, /Script/, /Temp/, /Transient/.
	 */
	static bool IsExportablePackage(const FString& PackagePath);

	/**
	 * Convert a Blueprint package path to the corresponding DSL file path.
	 * Convention: /Game/Path/AssetName -> {ProjectDir}/Saved/BP2DSL/{Category}/Path/AssetName.bplisp
	 * Also works with custom mount points: /MyPlugin/Path/AssetName -> .../{Category}/Path/AssetName.bplisp
	 *
	 * @param BlueprintPath  Package path, e.g. /Game/ALS/ALS_AnimBP
	 * @param CategoryTag    Category for subdirectory, e.g. "AnimBP"
	 * @param Extension      File extension, e.g. ".bplisp" or ".animlang"
	 * @return Absolute DSL file path, or empty string if BlueprintPath is invalid
	 */
	static FString BlueprintToDSLPath(
		const FString& BlueprintPath,
		const FString& CategoryTag = TEXT("AnimBP"),
		const FString& Extension = TEXT(".bplisp"));

	/**
	 * Convert a DSL file path back to the Blueprint package path.
	 * Inverse of BlueprintToDSLPath().
	 *
	 * Resolution strategy:
	 * 1. Extract asset name from DSL file path, query AssetRegistry for exact match.
	 *    If exactly one matching AnimBlueprint is found, returns its real PackagePath
	 *    (preserving the original mount point, e.g. /MyPlugin/ALS/ALS_Npc).
	 * 2. If no match or ambiguous, falls back to /Game/ prefix and logs a Warning.
	 *
	 * @param DSLFilePath  Absolute DSL file path
	 * @param CategoryTag  Category that was used during forward conversion
	 * @return Package path (e.g. /Game/ALS/ALS_AnimBP), or empty string if not a valid DSL path
	 */
	static FString DSLToBlueprintPath(
		const FString& DSLFilePath,
		const FString& CategoryTag = TEXT("AnimBP"));

	// ========== Mutation ==========

	/**
	 * Get or create a mapping entry for the given Blueprint path.
	 * If the entry exists, returns it. Otherwise creates a new BPOnly entry.
	 */
	FBP2FPMappingEntry& GetOrCreateEntry(
		const FString& BlueprintPath,
		const FString& CategoryTag = TEXT("AnimBP"));

	/**
	 * Mark a mapping as successfully exported (BP -> DSL).
	 * Updates DSLContentHash, LastExportTime, and state.
	 */
	void MarkExported(const FString& BlueprintPath, const FString& DSLContent);

	/**
	 * Mark a mapping as successfully imported (DSL -> BP).
	 * Updates state to Synced.
	 */
	void MarkImported(const FString& DSLFilePath);

private:
	// Singleton
	FBP2FPMappingRegistry() = default;
	FBP2FPMappingRegistry(const FBP2FPMappingRegistry&) = delete;
	FBP2FPMappingRegistry& operator=(const FBP2FPMappingRegistry&) = delete;

	// Scan all AnimBlueprint assets via AssetRegistry
	void ScanBlueprints();

	// Scan existing DSL files from disk
	void ScanDSLFiles(const FString& CategoryTag = TEXT("AnimBP"));

	// Reconcile BP and DSL scans to set correct states
	void Reconcile();

	// ========== Data ==========
	/** All mapping entries, indexed linearly */
	TArray<FBP2FPMappingEntry> Entries;

	/** Quick lookup: BlueprintPath -> index in Entries array */
	TMap<FString, int32> BlueprintToIndex;

	/** Quick lookup: DSLFilePath -> index in Entries array */
	TMap<FString, int32> DSLFileToIndex;
};
