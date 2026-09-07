// Copyright EngineWorks. All Rights Reserved.
#include "Modules/ModuleManager.h"
class FUnrealAITransportModule final : public IModuleInterface
{
  public:
	void ShutdownModule() override
	{
		UE_LOG(LogTemp, Fatal,
			   TEXT("Explicit UnrealAITransport unload is unsupported while native SDK interfaces can be retained."));
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
IMPLEMENT_MODULE(FUnrealAITransportModule, UnrealAITransport)
