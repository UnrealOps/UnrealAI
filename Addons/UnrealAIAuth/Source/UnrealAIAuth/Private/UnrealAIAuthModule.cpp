// Copyright UnrealOps. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "UnrealAIAuth.h"
#include "Models/UnrealAIBuiltInProviders.h"
#include "Auth/UnrealAIApiKeyCredentialBroker.h"
#include "Misc/CoreDelegates.h"
#include "Auth/UnrealAIPlatformSecretStore.h"
#include "Auth/UnrealAICredentialStoreRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIAuth, Log, All);

class FUnrealAICredentialBrokerFactory final : public IUnrealAICredentialBrokerFactory
{
  public:
	bool CreateApiKeyBroker(TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections,
							TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store,
							TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
							const FUnrealAIApiKeyCredentialBinding &Binding,
							const FUnrealAIApiKeyCredentialBrokerConfig &Config,
							TSharedPtr<IUnrealAICredentialBroker, ESPMode::ThreadSafe> &OutBroker,
							FString &OutError) override
	{
		OutBroker.Reset();
		if (!Binding.ValidateShape(OutError) || !Config.ValidateShape(OutError))
		{
			return false;
		}
		auto Broker =
			MakeShared<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe>(Connections, Store, Clock, Config);
		if (!Broker->RegisterBinding(Binding, OutError))
		{
			Broker->BeginShutdown();
			return false;
		}
		OutBroker = Broker;
		return true;
	}
};
class FUnrealAIAuthModule final : public IUnrealAIAuthModule
{
  public:
	void StartupModule() override
	{
		IUnrealAICredentialBrokerFactory::Register(MakeShared<FUnrealAICredentialBrokerFactory, ESPMode::ThreadSafe>());
		RegisterUnrealAIBuiltInProviders();
		FCoreDelegates::OnEnginePreExit.AddStatic(&ShutdownUnrealAIBuiltInProviders);
		const FUnrealAIPlatformSecretStoreSelection Selection =
			FUnrealAIPlatformSecretStoreSelection::CurrentUserOAuth(UE_BUILD_SHIPPING != 0);
		const FName StoreName = FUnrealAIPlatformSecretStore::GetCompiledStoreName();
		const FUnrealAISecretStoreCapabilities Capabilities =
			FUnrealAIPlatformSecretStore::DescribeCompiledCapabilities(Selection);
		if (StoreName.IsNone() || !Capabilities.bAvailableInCurrentBuild)
		{
			return;
		}
		FString Error;
		if (!FUnrealAICredentialStoreRegistry::RegisterCredentialStoreCapabilities(StoreName, Capabilities, Error))
		{
			UE_LOG(LogUnrealAIAuth, Error,
				   TEXT("Platform credential-store capability registration failed: %s"), *Error);
		}
	}

	void ShutdownModule() override
	{
		// Auth interfaces and implementations can escape through shared
		// cross-module ownership. Until Core owns every such object lifetime,
		// explicit dylib unload is unsupported and must fail closed.
		UE_LOG(LogUnrealAIAuth, Fatal,
			   TEXT("Explicit UnrealAIAuth unload is unsupported while native credential interfaces can be retained."));
	}

	bool SupportsDynamicReloading() override
	{
		return false;
	}

	bool SupportsAutomaticShutdown() override
	{
		return false;
	}

	FUnrealAIAccountAuthProviderRegistry &GetAccountAuthProviderRegistry() override
	{
		return FUnrealAIAccountAuthProviderRegistry::GetGlobal();
	}

	FUnrealAIAccountCatalogRegistry &GetAccountCatalogRegistry() override
	{
		return FUnrealAIAccountCatalogRegistry::GetGlobal();
	}

	TSharedRef<const FUnrealAIAccountCatalogSnapshot, ESPMode::ThreadSafe> GetAccountCatalogSnapshot() const override
	{
		return FUnrealAIAccountCatalogRegistry::GetGlobal().CreateSnapshot();
	}
};

IMPLEMENT_MODULE(FUnrealAIAuthModule, UnrealAIAuth)
