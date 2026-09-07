#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUnrealAI, Log, All);

class FUnrealAIModule : public IModuleInterface
{
  public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	// Native request and continuation owners may outlive reflected clients.
	bool SupportsDynamicReloading() override
	{
		return false;
	}
	bool SupportsAutomaticShutdown() override
	{
		return false;
	}
};
