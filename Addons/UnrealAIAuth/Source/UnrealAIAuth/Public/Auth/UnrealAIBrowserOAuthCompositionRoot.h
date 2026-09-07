// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIAccountCatalog.h"
#include "Auth/UnrealAIAccountAuthProviderRegistry.h"
#include "Auth/UnrealAIBrowserOAuthAccountProvider.h"
#include "Auth/UnrealAIConnectionRegistry.h"
#include "Auth/UnrealAIOAuthAuthorizationIssuerHttp.h"
#include "Auth/UnrealAIOAuthLoopbackAuthorizationBrowser.h"
#include "CoreMinimal.h"

/** Bounded Auth-owned composition policy for one production browser-OAuth account vertical. */
struct UNREALAIAUTH_API FUnrealAIBrowserOAuthCompositionConfig final
{
	FUnrealAIBrowserOAuthAccountProviderConfig AccountProvider;
	FUnrealAIOAuthAuthorizationCoordinatorConfig AuthorizationCoordinator;
	FUnrealAIOAuthCredentialBrokerConfig CredentialBroker;
	FUnrealAIOAuthAuthorizationIssuerHttpConfig IssuerHttp;
	FUnrealAIOAuthLoopbackAuthorizationBrowserConfig LoopbackBrowser;
	FUnrealAIAccountCatalogEntry AccountCatalog;
	bool bStartStoredSessionRecovery = true;

	bool ValidateShape(FString &OutError) const;
};

/** Synchronous provider-module hook that freezes endpoint and connection descriptors under the minted authority. */
using FUnrealAIBrowserOAuthConnectionComposer = TFunctionRef<bool(
	const FUnrealAIProviderEndpointAuthority &Authority,
	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections, FString &OutError)>;

/**
 * Lifecycle and composition owner for a browser-OAuth account provider, its secure store, broker, retained
 * authorization coordinator, durable commit transaction, and production browser/issuer/JOSE kernel.
 *
 * Registration first mints the endpoint authority required by provider modules. The root then completes connection
 * and broker wiring synchronously, rolling back the exact registration on every failure before returning to its
 * caller. A successful factory return therefore exposes only a complete vertical.
 */
class UNREALAIAUTH_API FUnrealAIBrowserOAuthCompositionRoot final
{
  public:
	~FUnrealAIBrowserOAuthCompositionRoot();
	FUnrealAIBrowserOAuthCompositionRoot(const FUnrealAIBrowserOAuthCompositionRoot &) = delete;
	FUnrealAIBrowserOAuthCompositionRoot &operator=(const FUnrealAIBrowserOAuthCompositionRoot &) = delete;

	/**
	 * Production factory. It selects the reviewed platform OAuth store and constructs strict issuer HTTP, OpenSSL JOSE,
	 * the system-browser loopback adapter, kernel, coordinator, transaction, broker, provider, and registry authority.
	 * Registry must outlive the returned root so shutdown can withdraw the exact registration.
	 */
	static bool TryCreateAndRegister(
		const FUnrealAIBrowserOAuthCompositionConfig &Config, FUnrealAIAccountAuthProviderRegistry &Registry,
		FUnrealAIAccountCatalogRegistry &AccountCatalog, FUnrealAIBrowserOAuthConnectionComposer ComposeConnections,
		TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe> IssuerHttpClient,
		TSharedRef<IUnrealAIOAuthCredentialRefreshSource, ESPMode::ThreadSafe> RefreshSource,
		TSharedPtr<FUnrealAIBrowserOAuthCompositionRoot, ESPMode::ThreadSafe> &OutRoot,
		FUnrealAIProviderEndpointAuthority &OutAuthority, FUnrealAIProviderAccessError &OutError);

	/**
	 * Project/test seam for a reviewed external secure store or deterministic authorization executor. Ownership and all
	 * lifecycle invariants are otherwise identical to the production factory. Registry must outlive the returned root.
	 */
	static bool TryCreateAndRegisterWithExecutor(
		const FUnrealAIBrowserOAuthCompositionConfig &Config, FUnrealAIAccountAuthProviderRegistry &Registry,
		FUnrealAIAccountCatalogRegistry &AccountCatalog, FUnrealAIBrowserOAuthConnectionComposer ComposeConnections,
		TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> Executor,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
		TSharedRef<IUnrealAIOAuthCredentialRefreshSource, ESPMode::ThreadSafe> RefreshSource,
		TSharedPtr<FUnrealAIBrowserOAuthCompositionRoot, ESPMode::ThreadSafe> &OutRoot,
		FUnrealAIProviderEndpointAuthority &OutAuthority, FUnrealAIProviderAccessError &OutError);

	TSharedRef<FUnrealAIBrowserOAuthAccountProvider, ESPMode::ThreadSafe> GetAccountProvider() const;
	TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> GetCredentialBroker() const;
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> GetConnections() const;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> GetSecretStore() const;
	void BeginShutdown();
	bool IsShutdown() const;

  private:
	FUnrealAIBrowserOAuthCompositionRoot(
		FUnrealAIAccountAuthProviderRegistry &InRegistry, FUnrealAIAccountCatalogRegistry &InAccountCatalog,
		FName InSelectionAlias, TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
		TSharedRef<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe> InAuthorizationCoordinator,
		TSharedRef<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe> InTransaction,
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InCredentialBroker,
		TSharedRef<FUnrealAIBrowserOAuthAccountProvider, ESPMode::ThreadSafe> InAccountProvider);

	FUnrealAIAccountAuthProviderRegistry *Registry = nullptr;
	FUnrealAIAccountCatalogRegistry *AccountCatalog = nullptr;
	FName SelectionAlias;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> Executor;
	TSharedRef<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe> AuthorizationCoordinator;
	TSharedRef<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe> Transaction;
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> CredentialBroker;
	TSharedRef<FUnrealAIBrowserOAuthAccountProvider, ESPMode::ThreadSafe> AccountProvider;
	TAtomic<bool> bShutdown{false};
};
