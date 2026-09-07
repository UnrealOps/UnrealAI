// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIAccountCatalog.h"
#include "Auth/UnrealAIDeviceOAuthAccountProvider.h"
#include "CoreMinimal.h"

/** Bounded Auth-owned composition policy for one device-OAuth account vertical. */
struct UNREALAIAUTH_API FUnrealAIDeviceOAuthCompositionConfig final
{
	FUnrealAIDeviceOAuthAccountProviderConfig AccountProvider;
	FUnrealAIOAuthCredentialBrokerConfig CredentialBroker;
	FUnrealAIAccountCatalogEntry AccountCatalog;
	bool bStartStoredSessionRecovery = true;

	bool ValidateShape(FString &OutError) const;
};

/** Provider-module hook that freezes endpoint and connection descriptors under the minted authority. */
using FUnrealAIDeviceOAuthConnectionComposer = TFunctionRef<bool(
	const FUnrealAIProviderEndpointAuthority &Authority,
	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections, FString &OutError)>;

/**
 * Lifecycle owner for a device authorization driver, platform store, account provider, broker, exact connection,
 * shared provider registration, and shared non-secret account catalog registration.
 */
class UNREALAIAUTH_API FUnrealAIDeviceOAuthCompositionRoot final
{
  public:
	~FUnrealAIDeviceOAuthCompositionRoot();
	FUnrealAIDeviceOAuthCompositionRoot(const FUnrealAIDeviceOAuthCompositionRoot &) = delete;
	FUnrealAIDeviceOAuthCompositionRoot &operator=(const FUnrealAIDeviceOAuthCompositionRoot &) = delete;

	/** Selects the reviewed current-user OAuth store and composes one complete registered device-flow vertical. */
	static bool TryCreateAndRegister(const FUnrealAIDeviceOAuthCompositionConfig &Config,
									 FUnrealAIAccountAuthProviderRegistry &ProviderRegistry,
									 FUnrealAIAccountCatalogRegistry &AccountCatalog,
									 FUnrealAIDeviceOAuthConnectionComposer ComposeConnections,
									 TSharedRef<IUnrealAIDeviceOAuthAuthorizationDriver, ESPMode::ThreadSafe> Driver,
									 TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
									 TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> &OutRoot,
									 FUnrealAIProviderEndpointAuthority &OutAuthority,
									 FUnrealAIProviderAccessError &OutError);

	/** Project/test seam for a reviewed external secure store and deterministic clock. */
	static bool TryCreateAndRegisterWithStore(
		const FUnrealAIDeviceOAuthCompositionConfig &Config, FUnrealAIAccountAuthProviderRegistry &ProviderRegistry,
		FUnrealAIAccountCatalogRegistry &AccountCatalog, FUnrealAIDeviceOAuthConnectionComposer ComposeConnections,
		TSharedRef<IUnrealAIDeviceOAuthAuthorizationDriver, ESPMode::ThreadSafe> Driver,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
		TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> &OutRoot,
		FUnrealAIProviderEndpointAuthority &OutAuthority, FUnrealAIProviderAccessError &OutError);

	TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> GetAccountProvider() const;
	TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> GetCredentialBroker() const;
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> GetConnections() const;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> GetSecretStore() const;
	void BeginShutdown();
	bool IsShutdown() const;

  private:
	FUnrealAIDeviceOAuthCompositionRoot(
		FUnrealAIAccountAuthProviderRegistry &InProviderRegistry, FUnrealAIAccountCatalogRegistry &InAccountCatalog,
		const FName InSelectionAlias, TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InCredentialBroker,
		TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> InAccountProvider);

	FUnrealAIAccountAuthProviderRegistry *ProviderRegistry = nullptr;
	FUnrealAIAccountCatalogRegistry *AccountCatalog = nullptr;
	FName SelectionAlias;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> CredentialBroker;
	TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> AccountProvider;
	TAtomic<bool> bShutdown{false};
};
