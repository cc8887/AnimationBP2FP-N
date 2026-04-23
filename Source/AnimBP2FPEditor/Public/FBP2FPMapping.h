// FBP2FPMapping.h - Blueprint <-> DSL Mapping Types
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Auto-sync mode: controls the direction of automatic synchronization.
 * Only one direction can be active at a time to prevent infinite loops.
 */
UENUM()
enum class EBP2FPSyncMode : uint8
{
	/** No automatic sync */
	None,
	/** Blueprint compile -> auto export to DSL */
	BP2FP,
	/** DSL file change -> auto import to Blueprint */
	FP2BP
};

/**
 * Sync state of a single Blueprint <-> DSL pair.
 */
UENUM()
enum class EBP2FPSyncState : uint8
{
	/** Both BP and DSL exist and are in sync */
	Synced,
	/** Only Blueprint exists (never exported or DSL was deleted) */
	BPOnly,
	/** Only DSL file exists (Blueprint was deleted or not yet created) */
	DSLOnly,
	/** Both exist but content hash mismatch */
	OutOfSync
};

/**
 * Represents a single Blueprint <-> DSL mapping entry.
 */
struct ANIMBP2FPEDITOR_API FBP2FPMappingEntry
{
	/** Package path of the Blueprint asset, e.g. /Game/ALS/ALS_AnimBP */
	FString BlueprintPath;

	/** Absolute file path of the DSL file on disk */
	FString DSLFilePath;

	/** Category tag for grouping (e.g. "AnimBP", "MatBP", "BP") */
	FString CategoryTag;

	/** Last time this entry was exported (BP -> DSL) */
	FDateTime LastExportTime;

	/** MD5 hash of the last exported DSL content, for change detection */
	FString DSLContentHash;

	/** Whether the Blueprint asset currently exists on disk */
	bool bBlueprintExists = false;

	/** Whether the DSL file currently exists on disk */
	bool bDSLFileExists = false;

	/** Current sync state */
	EBP2FPSyncState State = EBP2FPSyncState::BPOnly;

	/** Default constructor */
	FBP2FPMappingEntry() = default;

	/** Construct with blueprint path and category */
	FBP2FPMappingEntry(const FString& InBPPath, const FString& InCategory)
		: BlueprintPath(InBPPath)
		, CategoryTag(InCategory)
		, bBlueprintExists(true)
		, bDSLFileExists(false)
		, State(EBP2FPSyncState::BPOnly)
	{}
};
