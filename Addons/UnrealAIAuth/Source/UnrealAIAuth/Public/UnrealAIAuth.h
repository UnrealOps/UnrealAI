// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIAccountCatalog.h"
#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

/** Auth-owned process registry shared by independently loaded account-provider modules. */
class UNREALAIAUTH_API IUnrealAIAuthModule : public IModuleInterface
{
  public:
	static IUnrealAIAuthModule &Get()
	{
		return FModuleManager::LoadModuleChecked<IUnrealAIAuthModule>(TEXT("UnrealAIAuth"));
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("UnrealAIAuth"));
	}

	/** Trusted provider modules register their complete composition roots here. */
	virtual FUnrealAIAccountAuthProviderRegistry &GetAccountAuthProviderRegistry() = 0;
	/** Non-secret exact account choices are published here only after provider and connection composition succeeds. */
	virtual FUnrealAIAccountCatalogRegistry &GetAccountCatalogRegistry() = 0;
	virtual TSharedRef<const FUnrealAIAccountCatalogSnapshot, ESPMode::ThreadSafe>
	GetAccountCatalogSnapshot() const = 0;
};
