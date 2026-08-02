// AnimBP2FPCompilerHook.cpp - Blueprint Compilation Hook Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPCompilerHook.h"
#include "AnimBPExporter.h"
#include "AnimBP2FPPythonBridge.h"
#include "FBP2FPMappingRegistry.h"
#include "AnimBP2FPSettings.h"

#include "Editor/EditorEngine.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Containers/Ticker.h"

#define LOCTEXT_NAMESPACE "AnimBP2FPCompilerHook"

// ========== Lifecycle ==========

FAnimBP2FPCompilerHook::FAnimBP2FPCompilerHook()
{
}

FAnimBP2FPCompilerHook::~FAnimBP2FPCompilerHook()
{
	Unregister();
}

void FAnimBP2FPCompilerHook::Register()
{
	if (bIsRegistered) return;

	// Use GEditor events for precise compile-time detection
	// OnBlueprintPreCompile has the Blueprint parameter, so we can track which BP is compiling
	PreCompileHandle = GEditor->OnBlueprintPreCompile().AddRaw(
		this, &FAnimBP2FPCompilerHook::OnBlueprintPreCompile);

	// OnBlueprintCompiled has no params in UE5.5, so we use the tracked blueprint from PreCompile
	PostCompileHandle = GEditor->OnBlueprintCompiled().AddRaw(
		this, &FAnimBP2FPCompilerHook::OnBlueprintCompiled);

	// Ticker for deferred processing
#if ENGINE_MAJOR_VERSION < 5
	TickerHandle = FTicker::GetCoreTicker().AddTicker(
#else
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
#endif
		FTickerDelegate::CreateRaw(this, &FAnimBP2FPCompilerHook::Tick), 0.5f);

	bIsRegistered = true;

	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPCompilerHook: Registered via GEditor->OnBlueprintPreCompile/Compiled."));
}

void FAnimBP2FPCompilerHook::Unregister()
{
	if (!bIsRegistered) return;

	if (PreCompileHandle.IsValid())
	{
		GEditor->OnBlueprintPreCompile().Remove(PreCompileHandle);
		PreCompileHandle.Reset();
	}

	if (PostCompileHandle.IsValid())
	{
		GEditor->OnBlueprintCompiled().Remove(PostCompileHandle);
		PostCompileHandle.Reset();
	}

	if (TickerHandle.IsValid())
	{
#if ENGINE_MAJOR_VERSION < 5
		FTicker::GetCoreTicker().RemoveTicker(TickerHandle);
#else
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
#endif
		TickerHandle.Reset();
	}

	bIsRegistered = false;

	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPCompilerHook: Unregistered."));
}

// ========== Compilation Callbacks ==========

void FAnimBP2FPCompilerHook::OnBlueprintPreCompile(UBlueprint* Blueprint)
{
	// Track which blueprint is about to compile
	// We'll use this in OnBlueprintCompiled since that event has no params
	CurrentlyCompilingBP = Blueprint;
}

void FAnimBP2FPCompilerHook::OnBlueprintCompiled()
{
	// Check if auto-sync is enabled in settings
	const UAnimBP2FPSettings* Settings = GetDefault<UAnimBP2FPSettings>();
	if (!Settings || Settings->AutoSyncMode != EBP2FPSyncMode::BP2FP)
	{
		CurrentlyCompilingBP.Reset();
		return;
	}

	// Get the blueprint that just compiled (tracked in PreCompile)
	UBlueprint* Blueprint = CurrentlyCompilingBP.Get();
	CurrentlyCompilingBP.Reset();

	if (!Blueprint)
	{
		return;
	}

	// Only process Animation Blueprints
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Blueprint);
	if (!AnimBP)
	{
		return;
	}

	// Enqueue for deferred processing
	FString AssetPath = AnimBP->GetPathName();
	PendingAssetPaths.Enqueue(AssetPath);

	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPCompilerHook: AnimBP compiled, queued for export: %s"), *AssetPath);
}

// ========== Ticker / Deferred Processing ==========

bool FAnimBP2FPCompilerHook::Tick(float DeltaTime)
{
	int32 Processed = 0;
	FString AssetPath;

	while (PendingAssetPaths.Dequeue(AssetPath) && Processed < MaxPerTick)
	{
		ProcessQueuedBlueprint(AssetPath);
		Processed++;
	}

	// Return true to keep ticking
	return true;
}

void FAnimBP2FPCompilerHook::ProcessQueuedBlueprint(const FString& AssetPath)
{
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FPCompilerHook: Processing compiled AnimBP: %s"), *AssetPath);

	// Load the blueprint asset
	UBlueprint* Blueprint = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath));
	if (!Blueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("AnimBP2FPCompilerHook: Failed to load blueprint: %s"), *AssetPath);
		return;
	}

	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Blueprint);
	if (!AnimBP)
	{
		UE_LOG(LogTemp, Warning, TEXT("AnimBP2FPCompilerHook: Not an AnimBlueprint: %s"), *AssetPath);
		return;
	}

	// Export AnimGraph to .animlang
	ExportAnimGraph(AnimBP);

	// Export EventGraph to .bplisp
	ExportEventGraph(AnimBP);
}

// ========== Export Functions ==========

void FAnimBP2FPCompilerHook::ExportAnimGraph(UAnimBlueprint* AnimBP)
{
	if (!AnimBP) return;

	FString PackagePath = AnimBP->GetPathName();

	// Get or create mapping entry
	FBP2FPMappingEntry& Entry = FBP2FPMappingRegistry::Get().GetOrCreateEntry(PackagePath, TEXT("AnimBP"));
	FString AnimLangPath = FBP2FPMappingRegistry::BlueprintToDSLPath(PackagePath, TEXT("AnimBP"), TEXT(".animlang"));

	if (AnimLangPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("AnimBP2FPCompilerHook: Cannot resolve DSL path for %s"), *PackagePath);
		return;
	}

	// Ensure directory exists
	FString Dir = FPaths::GetPath(AnimLangPath);
	if (!IFileManager::Get().DirectoryExists(*Dir))
	{
		IFileManager::Get().MakeDirectory(*Dir, true);
	}

	// Export AnimGraph to AnimLang DSL
#if WITH_EDITOR
	FAnimBPExporter::FExportOptions Options;
	Options.bPrettyPrint = true;
	Options.IndentSize = 2;

	FString DSLOutput = FAnimBPExporter::ExportWithOptions(AnimBP, Options);

	if (DSLOutput.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("AnimBP2FPCompilerHook: AnimGraph export produced empty output for %s"), *PackagePath);
		return;
	}

	// Write file
	FString FileContent = FString::Printf(
		TEXT(";; AnimLang DSL Export (auto-synced)\n")
		TEXT(";; Source: %s\n")
		TEXT(";; Generated by AnimBP2FP CompilerHook\n")
		TEXT(";;\n\n%s\n"),
		*PackagePath, *DSLOutput);

	if (FFileHelper::SaveStringToFile(FileContent, *AnimLangPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Log, TEXT("AnimBP2FPCompilerHook: Exported AnimGraph -> %s"), *AnimLangPath);
		FBP2FPMappingRegistry::Get().MarkExported(PackagePath, DSLOutput);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("AnimBP2FPCompilerHook: Failed to write %s"), *AnimLangPath);
	}
#endif // WITH_EDITOR
}

void FAnimBP2FPCompilerHook::ExportEventGraph(UAnimBlueprint* AnimBP)
{
	if (!AnimBP) return;

	FString PackagePath = AnimBP->GetPathName();

	// Get .bplisp path
	FString BPListPath = FBP2FPMappingRegistry::BlueprintToDSLPath(PackagePath, TEXT("AnimBP"), TEXT(".bplisp"));
	if (BPListPath.IsEmpty())
	{
		return;
	}

	// Ensure directory exists
	FString Dir = FPaths::GetPath(BPListPath);
	if (!IFileManager::Get().DirectoryExists(*Dir))
	{
		IFileManager::Get().MakeDirectory(*Dir, true);
	}

	// Use the PythonBridge to export EventGraph to BlueprintLisp
#if WITH_EDITOR
	FAnimBP2FPPythonResult Result = UAnimBP2FPPythonBridge::ExportEventGraphToFile(
		PackagePath,
		BPListPath,
		TEXT("EventGraph"),
		false,  // bIncludePositions
		true    // bStableIds
	);

	if (Result.bSuccess)
	{
		UE_LOG(LogTemp, Log, TEXT("AnimBP2FPCompilerHook: Exported EventGraph -> %s"), *BPListPath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("AnimBP2FPCompilerHook: EventGraph export failed for %s: %s"),
			*PackagePath, *Result.Message);
	}
#endif // WITH_EDITOR
}

#undef LOCTEXT_NAMESPACE
