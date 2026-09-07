// Copyright EngineWorks. All Rights Reserved.
#include "Modules/ModuleManager.h"
class FUnrealAIAccessModule final : public IModuleInterface
{
  public:
	void ShutdownModule() override
	{
		UE_LOG(LogTemp, Fatal,
			   TEXT("Explicit UnrealAIAccess unload is unsupported while native SDK interfaces can be retained."));
	}
	bool SupportsDynamicReloading() override
	{
		return false;
	}
	bool SupportsAutomaticShutdown() override
	{
		return false;
	}
};
IMPLEMENT_MODULE(FUnrealAIAccessModule, UnrealAIAccess)
