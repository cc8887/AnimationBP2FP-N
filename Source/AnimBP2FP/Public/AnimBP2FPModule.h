// AnimBP2FPModule.h - Runtime Module Interface
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class UObject;
class UEdGraph;
class UEdGraphNode;

namespace AnimBP2FPImportLifecycle
{
	enum class EImportLifecyclePhase : uint8
	{
		PreNodeChanges,
		PostNodeChanges,
		PrePropertyChanges,
		PostPropertyChanges,
		PreFinalize,
		PostFinalize,
	};

	enum class EImportNodeChangeType : uint8
	{
		Added,
		Modified,
		Removed,
	};

	enum class EImportPropertyChangeType : uint8
	{
		Added,
		Modified,
		Removed,
	};

	struct ANIMBP2FP_API FImportLifecycleContext
	{
		FGuid ImportSessionId;
		UObject* TargetAsset = nullptr;
		UEdGraph* TargetGraph = nullptr;
		FName ScopeName;
		bool bIsFullRebuild = false;
		bool bIsIncremental = false;
		bool bIsHeadless = false;
		bool bWillCompile = false;
		TSet<FName> RequestedBehaviors;
	};

	struct ANIMBP2FP_API FImportNodeChange
	{
		UEdGraphNode* Node = nullptr;
		EImportNodeChangeType ChangeType = EImportNodeChangeType::Added;
	};

	struct ANIMBP2FP_API FImportPropertyChange
	{
		UObject* TargetObject = nullptr;
		FName PropertyName;
		EImportPropertyChangeType ChangeType = EImportPropertyChangeType::Modified;
	};

	struct ANIMBP2FP_API FImportNodePhaseEvent
	{
		EImportLifecyclePhase Phase = EImportLifecyclePhase::PreNodeChanges;
		FImportLifecycleContext Context;
		TArray<FImportNodeChange> Changes;
	};

	struct ANIMBP2FP_API FImportPropertyPhaseEvent
	{
		EImportLifecyclePhase Phase = EImportLifecyclePhase::PrePropertyChanges;
		FImportLifecycleContext Context;
		TArray<FImportPropertyChange> Changes;
	};

	struct ANIMBP2FP_API FImportFinalizePhaseEvent
	{
		EImportLifecyclePhase Phase = EImportLifecyclePhase::PreFinalize;
		FImportLifecycleContext Context;
	};

	struct ANIMBP2FP_API FImportLifecycleHookHandle
	{
		FGuid Id;

		bool IsValid() const
		{
			return Id.IsValid();
		}

		friend bool operator==(const FImportLifecycleHookHandle& Lhs, const FImportLifecycleHookHandle& Rhs)
		{
			return Lhs.Id == Rhs.Id;
		}
	};

	class ANIMBP2FP_API IImportLifecycleHook
	{
	public:
		virtual ~IImportLifecycleHook() = default;

		virtual int32 GetPriority(EImportLifecyclePhase Phase) const
		{
			return 0;
		}

		virtual void OnNodePhase(const FImportNodePhaseEvent& Event) {}
		virtual void OnPropertyPhase(const FImportPropertyPhaseEvent& Event) {}
		virtual void OnFinalizePhase(const FImportFinalizePhaseEvent& Event) {}
	};
}

class ANIMBP2FP_API IAnimBP2FPImportHookHost
{
public:
	virtual ~IAnimBP2FPImportHookHost() = default;

	virtual AnimBP2FPImportLifecycle::FImportLifecycleHookHandle RegisterImportLifecycleHook(
		TSharedRef<AnimBP2FPImportLifecycle::IImportLifecycleHook> Hook) = 0;

	virtual void UnregisterImportLifecycleHook(
		AnimBP2FPImportLifecycle::FImportLifecycleHookHandle Handle) = 0;
};

class ANIMBP2FP_API FAnimBP2FPModule : public IModuleInterface, public IAnimBP2FPImportHookHost
{
public:
	static inline FAnimBP2FPModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FAnimBP2FPModule>("AnimBP2FP");
	}

	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("AnimBP2FP");
	}

	virtual AnimBP2FPImportLifecycle::FImportLifecycleHookHandle RegisterImportLifecycleHook(
		TSharedRef<AnimBP2FPImportLifecycle::IImportLifecycleHook> Hook) override;

	virtual void UnregisterImportLifecycleHook(
		AnimBP2FPImportLifecycle::FImportLifecycleHookHandle Handle) override;

	void BroadcastNodePhase(const AnimBP2FPImportLifecycle::FImportNodePhaseEvent& Event);
	void BroadcastPropertyPhase(const AnimBP2FPImportLifecycle::FImportPropertyPhaseEvent& Event);
	void BroadcastFinalizePhase(const AnimBP2FPImportLifecycle::FImportFinalizePhaseEvent& Event);

	struct FRegisteredImportHook
	{
		AnimBP2FPImportLifecycle::FImportLifecycleHookHandle Handle;
		TSharedRef<AnimBP2FPImportLifecycle::IImportLifecycleHook> Hook;
		int64 RegistrationOrder = 0;
	};

private:
	TArray<FRegisteredImportHook> RegisteredHooks;
	int64 NextRegistrationOrder = 0;
};
