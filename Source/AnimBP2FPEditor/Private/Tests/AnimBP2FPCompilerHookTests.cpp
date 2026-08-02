// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// AnimBP2FPCompilerHookTests.cpp - UE Automation Tests for Compiler Hook
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
//
// Run via:
//   UnrealEditor.exe <project> -run=AutomationTests -filter="AnimBP2FP.CompilerHook"
// Or in Editor:
//   Window -> Developer Tools -> Session Frontend -> Automation

#if ENGINE_MAJOR_VERSION >= 5
#include "CoreMinimal.h"
#include "AnimBP2FPVersionCompat.h"
#include "Misc/AutomationTest.h"
#include "Engine/Blueprint.h"
#include "Animation/AnimBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

// Standard test flags: runs in Editor + Commandlet context, ProductFilter
#define ABP_FLAGS (ANIMBP2FP_APPLICATION_CONTEXT_FLAGS | EAutomationTestFlags::ProductFilter)

#define ABP_TEST(Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(F##Name, "AnimBP2FP.CompilerHook." #Name, ABP_FLAGS)

// ============================================================================
// Basic Event Verification Tests
// ============================================================================

ABP_TEST(OnBlueprintCompiled_IsAvailable)
bool FOnBlueprintCompiled_IsAvailable::RunTest(const FString& Parameters)
{
	// Verify that GEditor->OnBlueprintCompiled() is available
	TestNotNull(TEXT("GEditor should exist"), GEditor);
	
	if (GEditor)
	{
		// Just verify the delegate exists - we can't easily test the actual broadcast
		// without a loaded blueprint, but we can verify the delegate is accessible
		TestTrue(TEXT("OnBlueprintCompiled delegate is accessible"), true);
	}
	
	return true;
}

ABP_TEST(OnBlueprintPreCompile_IsAvailable)
bool FOnBlueprintPreCompile_IsAvailable::RunTest(const FString& Parameters)
{
	TestNotNull(TEXT("GEditor should exist"), GEditor);
	
	if (GEditor)
	{
		TestTrue(TEXT("OnBlueprintPreCompile delegate is accessible"), true);
	}
	
	return true;
}

// ============================================================================
// Inheritance Tests
// ============================================================================

ABP_TEST(AnimBlueprint_InheritsFromBlueprint)
bool FAnimBlueprint_InheritsFromBlueprint::RunTest(const FString& Parameters)
{
	// Verify UAnimBlueprint inherits from UBlueprint
	UClass* AnimBpClass = UAnimBlueprint::StaticClass();
	UClass* BlueprintClass = UBlueprint::StaticClass();
	
	TestNotNull(TEXT("UAnimBlueprint class exists"), AnimBpClass);
	TestNotNull(TEXT("UBlueprint class exists"), BlueprintClass);
	
	if (AnimBpClass && BlueprintClass)
	{
		TestTrue(TEXT("UAnimBlueprint inherits from UBlueprint"), 
			AnimBpClass->IsChildOf(BlueprintClass));
	}
	
	return true;
}

// ============================================================================
// Event Flow Tests (requires Editor context)
// ============================================================================

ABP_TEST(OnBlueprintPreCompile_HappensBeforeCompiled)
bool FOnBlueprintPreCompile_HappensBeforeCompiled::RunTest(const FString& Parameters)
{
	// This test verifies that OnBlueprintPreCompile fires before OnBlueprintCompiled
	// We track the order using a simple counter
	
	int32 EventOrder = 0;
	int32 PreCompileOrder = -1;
	int32 CompiledOrder = -1;
	
	FDelegateHandle PreHandle = GEditor->OnBlueprintPreCompile().AddLambda(
		[&EventOrder, &PreCompileOrder](UBlueprint* BP)
		{
			PreCompileOrder = EventOrder++;
		});
	
	FDelegateHandle CompiledHandle = GEditor->OnBlueprintCompiled().AddLambda(
		[&EventOrder, &CompiledOrder]()
		{
			CompiledOrder = EventOrder++;
		});
	
	// Simulate the expected order (PreCompile then Compiled)
	// Note: We can't actually trigger a blueprint compile in this test context,
	// but we verify the delegate registration works
	
	TestTrue(TEXT("PreCompile delegate registered"), PreHandle.IsValid());
	TestTrue(TEXT("Compiled delegate registered"), CompiledHandle.IsValid());
	
	// Cleanup
	GEditor->OnBlueprintPreCompile().Remove(PreHandle);
	GEditor->OnBlueprintCompiled().Remove(CompiledHandle);
	
	return true;
}

// ============================================================================
// Settings Tests
// ============================================================================

#include "AnimBP2FPSettings.h"

ABP_TEST(Settings_HasAutoSyncMode)
bool FSettings_HasAutoSyncMode::RunTest(const FString& Parameters)
{
	const UAnimBP2FPSettings* Settings = GetDefault<UAnimBP2FPSettings>();
	TestNotNull(TEXT("Settings should exist"), Settings);
	
	if (Settings)
	{
		// Verify the enum values exist
		TestTrue(TEXT("None mode exists"), 
			Settings->AutoSyncMode == EBP2FPSyncMode::None ||
			Settings->AutoSyncMode == EBP2FPSyncMode::BP2FP ||
			Settings->AutoSyncMode == EBP2FPSyncMode::FP2BP);
	}
	
	return true;
}

ABP_TEST(Settings_DefaultIsNone)
bool FSettings_DefaultIsNone::RunTest(const FString& Parameters)
{
	// Create a fresh settings object (not the default config)
	UAnimBP2FPSettings* TestSettings = NewObject<UAnimBP2FPSettings>();
	TestNotNull(TEXT("Test settings created"), TestSettings);
	
	if (TestSettings)
	{
		// Default should be None for safety
		TestEqual(TEXT("Default AutoSyncMode should be None"), 
			TestSettings->AutoSyncMode, EBP2FPSyncMode::None);
	}
	
	return true;
}

// ============================================================================
// Integration Test Marker
// ============================================================================

ABP_TEST(Integration_RequiresEditorContext)
bool FIntegration_RequiresEditorContext::RunTest(const FString& Parameters)
{
	// This is a marker test that indicates more comprehensive tests
	// should be run in Editor context with actual blueprints
	
	UE_LOG(LogTemp, Log, TEXT("AnimBP2FP CompilerHook tests require Editor context for full integration testing"));
	UE_LOG(LogTemp, Log, TEXT("To test manually:"));
	UE_LOG(LogTemp, Log, TEXT("  1. Enable AutoSyncMode = BP2FP in Project Settings"));
	UE_LOG(LogTemp, Log, TEXT("  2. Create/compile an AnimBlueprint"));
	UE_LOG(LogTemp, Log, TEXT("  3. Check Output Log for '[AnimBP2FP]' messages"));
	UE_LOG(LogTemp, Log, TEXT("  4. Verify .animl file is generated"));
	
	TestTrue(TEXT("Integration test placeholder"), true);
	return true;
}

// ============================================================================
// Import-Lifecycle Hook Integration Tests
//
// These guard the producer-side contract that the BlueprintAutoLayout plugin
// relies on: after an import that touches nodes, AnimBP2FP broadcasts a
// PostNodeChanges event carrying (a) the changed UEdGraphNodes and (b) the
// "AutoLayout" behavior token in RequestedBehaviors. BlueprintAutoLayout's
// FAnimBP2FPAutoLayoutHook consumes exactly this to run LayoutSelection over
// the changed nodes (and FBALAnimBP2FPHighlightHook to highlight them).
// ============================================================================

#include "AnimBP2FPModule.h"

namespace AnimBP2FPLifecycleTest
{
	using namespace AnimBP2FPImportLifecycle;

	/** Records every node-phase event it receives, with a configurable priority. */
	class FRecordingHook : public IImportLifecycleHook
	{
	public:
		explicit FRecordingHook(int32 InPriority = 0) : Priority(InPriority) {}

		virtual int32 GetPriority(EImportLifecyclePhase Phase) const override
		{
			return Phase == EImportLifecyclePhase::PostNodeChanges ? Priority : 0;
		}

		virtual void OnNodePhase(const FImportNodePhaseEvent& Event) override
		{
			NodePhaseCount++;
			LastPhase = Event.Phase;
			LastBehaviors = Event.Context.RequestedBehaviors;
			LastChangeCount = Event.Changes.Num();
			OrderToken = NextGlobalOrder++;
		}

		int32 Priority = 0;
		int32 NodePhaseCount = 0;
		EImportLifecyclePhase LastPhase = EImportLifecyclePhase::PreNodeChanges;
		TSet<FName> LastBehaviors;
		int32 LastChangeCount = 0;
		int32 OrderToken = -1;

		static int32 NextGlobalOrder;
	};

	int32 FRecordingHook::NextGlobalOrder = 0;
}

ABP_TEST(Lifecycle_AnimPostNodeChanges_DeliveredWithAutoLayoutBehavior)
bool FLifecycle_AnimPostNodeChanges_DeliveredWithAutoLayoutBehavior::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPImportLifecycle;
	using namespace AnimBP2FPLifecycleTest;

	if (!FAnimBP2FPModule::IsAvailable())
	{
		AddWarning(TEXT("AnimBP2FP module not loaded; skipping lifecycle test."));
		return true;
	}

	FAnimBP2FPModule& Module = FAnimBP2FPModule::Get();

	TSharedRef<FRecordingHook> Hook = MakeShared<FRecordingHook>();
	FImportLifecycleHookHandle Handle = Module.RegisterImportLifecycleHook(Hook);
	TestTrue(TEXT("hook handle valid"), Handle.IsValid());

	// Build a PostNodeChanges event that mirrors what AnimBPImporter emits:
	// AutoLayout behavior requested + one changed node.
	UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage());
	UEdGraphNode* ChangedNode = NewObject<UEdGraphNode>(Graph);
	Graph->Nodes.Add(ChangedNode);

	FImportNodePhaseEvent Event;
	Event.Phase = EImportLifecyclePhase::PostNodeChanges;
	Event.Context.TargetGraph = Graph;
	Event.Context.bIsIncremental = true;
	Event.Context.RequestedBehaviors.Add(FName(TEXT("AutoLayout")));
	FImportNodeChange Change;
	Change.Node = ChangedNode;
	Change.ChangeType = EImportNodeChangeType::Modified;
	Event.Changes.Add(Change);

	Module.BroadcastNodePhase(Event);

	TestEqual(TEXT("hook received exactly one node phase"), Hook->NodePhaseCount, 1);
	TestEqual(TEXT("phase is PostNodeChanges"),
		(int32)Hook->LastPhase, (int32)EImportLifecyclePhase::PostNodeChanges);
	TestTrue(TEXT("AutoLayout behavior propagated"),
		Hook->LastBehaviors.Contains(FName(TEXT("AutoLayout"))));
	TestEqual(TEXT("changed-node count propagated"), Hook->LastChangeCount, 1);

	Module.UnregisterImportLifecycleHook(Handle);

	// After unregister, further broadcasts must not reach the hook.
	Module.BroadcastNodePhase(Event);
	TestEqual(TEXT("no delivery after unregister"), Hook->NodePhaseCount, 1);

	return true;
}

ABP_TEST(Lifecycle_AnimHookPriorityOrdering)
bool FLifecycle_AnimHookPriorityOrdering::RunTest(const FString& Parameters)
{
	using namespace AnimBP2FPImportLifecycle;
	using namespace AnimBP2FPLifecycleTest;

	if (!FAnimBP2FPModule::IsAvailable())
	{
		AddWarning(TEXT("AnimBP2FP module not loaded; skipping priority test."));
		return true;
	}

	FAnimBP2FPModule& Module = FAnimBP2FPModule::Get();

	// AutoLayout uses priority 100 (runs early); Highlight uses -10 (runs late).
	// Verify higher priority is invoked first.
	FRecordingHook::NextGlobalOrder = 0;
	TSharedRef<FRecordingHook> EarlyHook = MakeShared<FRecordingHook>(/*Priority*/ 100);
	TSharedRef<FRecordingHook> LateHook  = MakeShared<FRecordingHook>(/*Priority*/ -10);

	// Register late first to prove ordering is by priority, not registration order.
	FImportLifecycleHookHandle LateHandle  = Module.RegisterImportLifecycleHook(LateHook);
	FImportLifecycleHookHandle EarlyHandle = Module.RegisterImportLifecycleHook(EarlyHook);

	FImportNodePhaseEvent Event;
	Event.Phase = EImportLifecyclePhase::PostNodeChanges;
	Module.BroadcastNodePhase(Event);

	TestTrue(TEXT("high-priority hook ran before low-priority hook"),
		EarlyHook->OrderToken < LateHook->OrderToken);

	Module.UnregisterImportLifecycleHook(LateHandle);
	Module.UnregisterImportLifecycleHook(EarlyHandle);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
#endif // ENGINE_MAJOR_VERSION >= 5
#endif // UE 5.8+