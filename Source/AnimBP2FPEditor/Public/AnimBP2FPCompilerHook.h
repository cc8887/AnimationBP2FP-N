// AnimBP2FPCompilerHook.h - Blueprint Compilation Hook for Auto-Sync
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UAnimBlueprint;

/**
 * Hooks into blueprint compilation events to automatically export
 * Animation Blueprints to DSL when they are compiled.
 *
 * Uses GEditor->OnBlueprintCompiled() for precise compile-time detection.
 * Since OnBlueprintCompiled is a no-param event in UE5.5, we track the
 * currently compiling blueprint via OnBlueprintPreCompile.
 *
 * Uses TQueue + FTicker for deferred processing to avoid blocking
 * the compile pipeline during batch operations.
 */
class ANIMBP2FPEDITOR_API FAnimBP2FPCompilerHook
{
public:
	FAnimBP2FPCompilerHook();
	~FAnimBP2FPCompilerHook();

	/** Register the compilation hooks */
	void Register();

	/** Unregister the compilation hooks */
	void Unregister();

	/** Check if currently registered */
	bool IsRegistered() const { return bIsRegistered; }

private:
	/** Callback: Called before a blueprint compiles (has Blueprint param) */
	void OnBlueprintPreCompile(UBlueprint* Blueprint);

	/** Callback: Called after any blueprint compiles (no params in UE5.5) */
	void OnBlueprintCompiled();

	/** Ticker callback for deferred processing */
	bool Tick(float DeltaTime);

	/** Process a single blueprint from the queue */
	void ProcessQueuedBlueprint(const FString& AssetPath);

	/** Export AnimGraph to .animlang file */
	void ExportAnimGraph(UAnimBlueprint* AnimBP);

	/** Export EventGraph to .bplisp file */
	void ExportEventGraph(UAnimBlueprint* AnimBP);

	// Delegate handles
	FDelegateHandle PreCompileHandle;
	FDelegateHandle PostCompileHandle;
	FTSTicker::FDelegateHandle TickerHandle;

	// Registration state
	bool bIsRegistered = false;

	// Track the currently compiling blueprint (set in PreCompile, used in PostCompile)
	TWeakObjectPtr<UBlueprint> CurrentlyCompilingBP;

	// Deferred processing queue (thread-safe)
	TQueue<FString> PendingAssetPaths;

	// Process at most N per tick to avoid frame stalls
	static constexpr int32 MaxPerTick = 5;
};
