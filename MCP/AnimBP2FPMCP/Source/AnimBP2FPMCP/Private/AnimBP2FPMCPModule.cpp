// AnimBP2FPMCPModule.cpp - Toolset Registry lifecycle for AnimBP2FP

#include "Modules/ModuleManager.h"
#include "AnimBP2FPVersionCompat.h"

#if ANIMBP2FP_HAS_MODERN_RIGVM_AUTHORING

#include "AnimBP2FPMCPToolsetImpl.h"
#include "Misc/CoreDelegates.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FAnimBP2FPMCPModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RegisterToolset();
		if (!bToolsetRegistered)
		{
			PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
				this, &FAnimBP2FPMCPModule::RegisterToolset);
		}
	}

	virtual void ShutdownModule() override
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}

		if (bToolsetRegistered && UToolsetRegistry::IsAvailable())
		{
			UToolsetRegistry::UnregisterToolsetClass(UAnimBP2FPToolset::StaticClass());
		}
		bToolsetRegistered = false;
	}

private:
	void RegisterToolset()
	{
		if (bToolsetRegistered)
		{
			return;
		}

		if (!UToolsetRegistry::IsAvailable())
		{
			UE_LOG(LogTemp, Verbose, TEXT("AnimBP2FPMCP: ToolsetRegistry is not ready; waiting for post-engine init."));
			return;
		}

		UToolsetRegistry::RegisterToolsetClass(UAnimBP2FPToolset::StaticClass());
		bToolsetRegistered = UToolsetRegistry::IsToolsetClassRegistered(UAnimBP2FPToolset::StaticClass());
	}

	FDelegateHandle PostEngineInitHandle;
	bool bToolsetRegistered = false;
};

IMPLEMENT_MODULE(FAnimBP2FPMCPModule, AnimBP2FPMCP)

#else

IMPLEMENT_MODULE(FDefaultModuleImpl, AnimBP2FPMCP)

#endif
