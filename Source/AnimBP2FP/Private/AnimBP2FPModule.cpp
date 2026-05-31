// AnimBP2FPModule.cpp - Runtime Module Implementation
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#include "AnimBP2FPModule.h"
#include "Algo/Sort.h"

namespace
{
	using namespace AnimBP2FPImportLifecycle;

	template <typename EventType, typename CallbackType>
	void ABP_SortAndBroadcastHooks(
		const TArray<FAnimBP2FPModule::FRegisteredImportHook>& RegisteredHooks,
		EImportLifecyclePhase Phase,
		const EventType& Event,
		CallbackType&& Callback)
	{
		TArray<FAnimBP2FPModule::FRegisteredImportHook> Hooks = RegisteredHooks;
		Algo::Sort(Hooks, [Phase](const FAnimBP2FPModule::FRegisteredImportHook& Lhs,
			const FAnimBP2FPModule::FRegisteredImportHook& Rhs)
		{
			const int32 LhsPriority = Lhs.Hook->GetPriority(Phase);
			const int32 RhsPriority = Rhs.Hook->GetPriority(Phase);
			if (LhsPriority != RhsPriority)
			{
				return LhsPriority > RhsPriority;
			}
			return Lhs.RegistrationOrder < Rhs.RegistrationOrder;
		});

		for (const FAnimBP2FPModule::FRegisteredImportHook& Entry : Hooks)
		{
			Callback(*Entry.Hook, Event);
		}
	}
}

AnimBP2FPImportLifecycle::FImportLifecycleHookHandle FAnimBP2FPModule::RegisterImportLifecycleHook(
	TSharedRef<AnimBP2FPImportLifecycle::IImportLifecycleHook> Hook)
{
	AnimBP2FPImportLifecycle::FImportLifecycleHookHandle Handle;
	Handle.Id = FGuid::NewGuid();

	FRegisteredImportHook Entry{Handle, Hook, NextRegistrationOrder++};
	RegisteredHooks.Add(MoveTemp(Entry));
	return Handle;
}

void FAnimBP2FPModule::UnregisterImportLifecycleHook(
	AnimBP2FPImportLifecycle::FImportLifecycleHookHandle Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	RegisteredHooks.RemoveAll([&Handle](const FRegisteredImportHook& Entry)
	{
		return Entry.Handle == Handle;
	});
}

void FAnimBP2FPModule::BroadcastNodePhase(const AnimBP2FPImportLifecycle::FImportNodePhaseEvent& Event)
{
	ABP_SortAndBroadcastHooks(RegisteredHooks, Event.Phase, Event,
		[](AnimBP2FPImportLifecycle::IImportLifecycleHook& Hook,
			const AnimBP2FPImportLifecycle::FImportNodePhaseEvent& InEvent)
		{
			Hook.OnNodePhase(InEvent);
		});
}

void FAnimBP2FPModule::BroadcastPropertyPhase(const AnimBP2FPImportLifecycle::FImportPropertyPhaseEvent& Event)
{
	ABP_SortAndBroadcastHooks(RegisteredHooks, Event.Phase, Event,
		[](AnimBP2FPImportLifecycle::IImportLifecycleHook& Hook,
			const AnimBP2FPImportLifecycle::FImportPropertyPhaseEvent& InEvent)
		{
			Hook.OnPropertyPhase(InEvent);
		});
}

void FAnimBP2FPModule::BroadcastFinalizePhase(const AnimBP2FPImportLifecycle::FImportFinalizePhaseEvent& Event)
{
	ABP_SortAndBroadcastHooks(RegisteredHooks, Event.Phase, Event,
		[](AnimBP2FPImportLifecycle::IImportLifecycleHook& Hook,
			const AnimBP2FPImportLifecycle::FImportFinalizePhaseEvent& InEvent)
		{
			Hook.OnFinalizePhase(InEvent);
		});
}

IMPLEMENT_MODULE(FAnimBP2FPModule, AnimBP2FP)
