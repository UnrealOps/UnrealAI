// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIBrowserOAuthCompositionRoot.h"

#include "Auth/UnrealAIOAuthAuthorizationKernel.h"
#include "Auth/UnrealAIOAuthOpenSslAuthorizationCrypto.h"
#include "Auth/UnrealAIPlatformSecretStore.h"
#include "Runtime/UnrealAIClock.h"

namespace
{
FUnrealAIProviderAccessError MakeCompositionError(const EUnrealAIErrorCategory Category,
												  const EUnrealAIProviderAccessErrorCode Code)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	return Error;
}

bool HasExactBrowserOAuthConnection(const FUnrealAIConnectionRegistrySnapshot &Connections,
									const FUnrealAIBrowserOAuthAccountProviderConfig &Config,
									const FUnrealAIAccountCatalogEntry &CatalogEntry)
{
	const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection =
		Connections.Find(CatalogEntry.ConnectionAlias);
	if (Connection.IsValid())
	{
		const FUnrealAICredentialDestination &Destination = Connection->CredentialDestination;
		if (Destination.ModelProviderName == Config.ModelProviderName &&
			Destination.AccountAuthProviderName == Config.ProviderName &&
			Destination.AuthProfileId == Config.AuthProfileId && Destination.AccountId == Config.AccountId &&
			Destination.Audience == Config.Authorization.Audience &&
			Destination.AuthScheme == EUnrealAIAuthScheme::OAuthBearer &&
			Destination.BillingMode == EUnrealAIBillingMode::SubscriptionQuota)
		{
			return true;
		}
	}
	return false;
}
} // namespace

bool FUnrealAIBrowserOAuthCompositionConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString NestedError;
	if (!AccountProvider.ValidateShape(NestedError) || !AuthorizationCoordinator.ValidateShape(NestedError) ||
		!CredentialBroker.ValidateShape(NestedError) || !IssuerHttp.ValidateShape(NestedError) ||
		!LoopbackBrowser.ValidateShape(NestedError) || !AccountCatalog.ValidateShape(NestedError) ||
		AccountCatalog.ProviderName != AccountProvider.ProviderName ||
		AccountCatalog.AuthProfileId != AccountProvider.AuthProfileId ||
		!(AccountCatalog.AccountId == AccountProvider.AccountId))
	{
		OutError = TEXT("Browser OAuth composition configuration is invalid.");
		return false;
	}
	return true;
}

FUnrealAIBrowserOAuthCompositionRoot::FUnrealAIBrowserOAuthCompositionRoot(
	FUnrealAIAccountAuthProviderRegistry &InRegistry, FUnrealAIAccountCatalogRegistry &InAccountCatalog,
	const FName InSelectionAlias, TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
	TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
	TSharedRef<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe> InAuthorizationCoordinator,
	TSharedRef<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe> InTransaction,
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
	TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InCredentialBroker,
	TSharedRef<FUnrealAIBrowserOAuthAccountProvider, ESPMode::ThreadSafe> InAccountProvider)
	: Registry(&InRegistry), AccountCatalog(&InAccountCatalog), SelectionAlias(InSelectionAlias),
	  Clock(MoveTemp(InClock)), SecretStore(MoveTemp(InSecretStore)), Executor(MoveTemp(InExecutor)),
	  AuthorizationCoordinator(MoveTemp(InAuthorizationCoordinator)), Transaction(MoveTemp(InTransaction)),
	  Connections(MoveTemp(InConnections)), CredentialBroker(MoveTemp(InCredentialBroker)),
	  AccountProvider(MoveTemp(InAccountProvider))
{
}

FUnrealAIBrowserOAuthCompositionRoot::~FUnrealAIBrowserOAuthCompositionRoot()
{
	BeginShutdown();
}

bool FUnrealAIBrowserOAuthCompositionRoot::TryCreateAndRegister(
	const FUnrealAIBrowserOAuthCompositionConfig &Config, FUnrealAIAccountAuthProviderRegistry &Registry,
	FUnrealAIAccountCatalogRegistry &AccountCatalog, FUnrealAIBrowserOAuthConnectionComposer ComposeConnections,
	TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe> IssuerHttpClient,
	TSharedRef<IUnrealAIOAuthCredentialRefreshSource, ESPMode::ThreadSafe> RefreshSource,
	TSharedPtr<FUnrealAIBrowserOAuthCompositionRoot, ESPMode::ThreadSafe> &OutRoot,
	FUnrealAIProviderEndpointAuthority &OutAuthority, FUnrealAIProviderAccessError &OutError)
{
	OutRoot.Reset();
	OutAuthority = {};
	OutError = {};
	FString ShapeError;
	if (!Config.ValidateShape(ShapeError))
	{
		OutError = MakeCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
										EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	if (!FUnrealAIOAuthOpenSslAuthorizationCrypto::IsPlatformSupported())
	{
		OutError = MakeCompositionError(EUnrealAIErrorCategory::UnsupportedCapability,
										EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
		return false;
	}

	TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	FUnrealAISecretStoreCapabilities StoreCapabilities;
	if (!FUnrealAIPlatformSecretStore::TryCreate(
			FUnrealAIPlatformSecretStoreSelection::CurrentUserOAuth(UE_BUILD_SHIPPING != 0), SecretStore,
			StoreCapabilities, OutError) ||
		!SecretStore.IsValid() || Config.AccountProvider.SecretHandle.StoreName != SecretStore->GetStoreName())
	{
		if (!OutError.IsError())
		{
			OutError = MakeCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
											EUnrealAIProviderAccessErrorCode::SecretHandleStoreMismatch);
		}
		return false;
	}

	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAISystemClock, ESPMode::ThreadSafe>();
	const TSharedRef<IUnrealAIOAuthAuthorizationCrypto, ESPMode::ThreadSafe> Crypto =
		MakeShared<FUnrealAIOAuthOpenSslAuthorizationCrypto, ESPMode::ThreadSafe>();
	const TSharedRef<IUnrealAIOAuthAuthorizationIssuer, ESPMode::ThreadSafe> Issuer =
		MakeShared<FUnrealAIOAuthAuthorizationIssuerHttp, ESPMode::ThreadSafe>(IssuerHttpClient, Clock,
																			   Config.IssuerHttp);
	const TSharedRef<IUnrealAIOAuthAuthorizationBrowser, ESPMode::ThreadSafe> Browser =
		MakeShared<FUnrealAIOAuthLoopbackAuthorizationBrowser, ESPMode::ThreadSafe>(Config.LoopbackBrowser);
	const TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> Executor =
		MakeShared<FUnrealAIOAuthAuthorizationKernel, ESPMode::ThreadSafe>(Crypto, Issuer, Browser, Clock);
	return TryCreateAndRegisterWithExecutor(Config, Registry, AccountCatalog, ComposeConnections, Executor,
											SecretStore.ToSharedRef(), Clock, MoveTemp(RefreshSource), OutRoot,
											OutAuthority, OutError);
}

bool FUnrealAIBrowserOAuthCompositionRoot::TryCreateAndRegisterWithExecutor(
	const FUnrealAIBrowserOAuthCompositionConfig &Config, FUnrealAIAccountAuthProviderRegistry &Registry,
	FUnrealAIAccountCatalogRegistry &AccountCatalog, FUnrealAIBrowserOAuthConnectionComposer ComposeConnections,
	TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> Executor,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
	TSharedRef<IUnrealAIOAuthCredentialRefreshSource, ESPMode::ThreadSafe> RefreshSource,
	TSharedPtr<FUnrealAIBrowserOAuthCompositionRoot, ESPMode::ThreadSafe> &OutRoot,
	FUnrealAIProviderEndpointAuthority &OutAuthority, FUnrealAIProviderAccessError &OutError)
{
	OutRoot.Reset();
	OutAuthority = {};
	OutError = {};
	FString SetupError;
	if (!Config.ValidateShape(SetupError) || RefreshSource->GetProviderName() != Config.AccountProvider.ProviderName ||
		Config.AccountProvider.SecretHandle.StoreName != SecretStore->GetStoreName())
	{
		OutError = MakeCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
										EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	const TSharedRef<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe> Coordinator =
		MakeShared<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe>(Executor, Clock,
																				Config.AuthorizationCoordinator);
	const TSharedRef<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe> Transaction =
		MakeShared<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe>(Executor, SecretStore, Clock);
	if (!Transaction->IsAvailable())
	{
		Coordinator->BeginShutdown();
		OutError = MakeCompositionError(EUnrealAIErrorCategory::UnsupportedCapability,
										EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
		return false;
	}

	const TSharedRef<FUnrealAIBrowserOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIBrowserOAuthAccountProvider, ESPMode::ThreadSafe>(Config.AccountProvider, Transaction,
																			  Coordinator, SecretStore, Clock);
	if (!Registry.Register(Provider, OutAuthority, SetupError) || !OutAuthority.IsValid())
	{
		Provider->BeginShutdown();
		Coordinator->BeginShutdown();
		OutAuthority = {};
		OutError = MakeCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
										EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	if (!ComposeConnections(OutAuthority, Connections, SetupError) || !Connections.IsValid() ||
		!HasExactBrowserOAuthConnection(*Connections, Config.AccountProvider, Config.AccountCatalog))
	{
		FString RollbackError;
		Registry.Unregister(Provider, RollbackError);
		Provider->BeginShutdown();
		Coordinator->BeginShutdown();
		OutAuthority = {};
		OutError = MakeCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
										EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	const TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker =
		MakeShared<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe>(Connections.ToSharedRef(), SecretStore, Clock,
																		RefreshSource, Config.CredentialBroker);
	FUnrealAIOAuthCredentialBinding Binding;
	Binding.ProviderName = Config.AccountProvider.ProviderName;
	Binding.AuthProfileId = Config.AccountProvider.AuthProfileId;
	Binding.AccountId = Config.AccountProvider.AccountId;
	Binding.SecretHandle = Config.AccountProvider.SecretHandle;
	Binding.bRequiresProtectedSecondary = Config.AccountProvider.bRequireAccountRoutingValue;
	if (!Broker->RegisterBinding(Binding, SetupError))
	{
		FString RollbackError;
		Registry.Unregister(Provider, RollbackError);
		Provider->BeginShutdown();
		Broker->BeginShutdown();
		Coordinator->BeginShutdown();
		OutAuthority = {};
		OutError = MakeCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
										EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	if (!Provider->SetCredentialBroker(Broker, SetupError))
	{
		FString RollbackError;
		Registry.Unregister(Provider, RollbackError);
		Provider->BeginShutdown();
		Broker->BeginShutdown();
		Coordinator->BeginShutdown();
		OutAuthority = {};
		OutError = MakeCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
										EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> CatalogConnection =
		Connections->Find(Config.AccountCatalog.ConnectionAlias);
	if (!CatalogConnection.IsValid() ||
		!AccountCatalog.Register(Config.AccountCatalog, Provider, CatalogConnection.ToSharedRef(), SetupError))
	{
		FString RollbackError;
		Registry.Unregister(Provider, RollbackError);
		Provider->BeginShutdown();
		Broker->BeginShutdown();
		Coordinator->BeginShutdown();
		OutAuthority = {};
		OutError = MakeCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
										EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	const TSharedRef<FUnrealAIBrowserOAuthCompositionRoot, ESPMode::ThreadSafe> Candidate =
		MakeShareable(new FUnrealAIBrowserOAuthCompositionRoot(
			Registry, AccountCatalog, Config.AccountCatalog.SelectionAlias, Clock, SecretStore, Executor, Coordinator,
			Transaction, Connections.ToSharedRef(), Broker, Provider));

	OutRoot = Candidate;
	if (Config.bStartStoredSessionRecovery)
	{
		Provider->StartStoredSessionRecovery();
	}
	return true;
}

TSharedRef<FUnrealAIBrowserOAuthAccountProvider, ESPMode::ThreadSafe>
FUnrealAIBrowserOAuthCompositionRoot::GetAccountProvider() const
{
	return AccountProvider;
}

TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe>
FUnrealAIBrowserOAuthCompositionRoot::GetCredentialBroker() const
{
	return CredentialBroker;
}

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe>
FUnrealAIBrowserOAuthCompositionRoot::GetConnections() const
{
	return Connections;
}

TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> FUnrealAIBrowserOAuthCompositionRoot::GetSecretStore() const
{
	return SecretStore;
}

void FUnrealAIBrowserOAuthCompositionRoot::BeginShutdown()
{
	if (bShutdown.Exchange(true))
	{
		return;
	}
	if (AccountCatalog != nullptr)
	{
		FString UnregisterError;
		AccountCatalog->Unregister(SelectionAlias, AccountProvider, UnregisterError);
		AccountCatalog = nullptr;
	}
	AccountProvider->BeginShutdown();
	CredentialBroker->BeginShutdown();
	AuthorizationCoordinator->BeginShutdown();
	if (Registry != nullptr)
	{
		FString UnregisterError;
		Registry->Unregister(AccountProvider, UnregisterError);
		Registry = nullptr;
	}
}

bool FUnrealAIBrowserOAuthCompositionRoot::IsShutdown() const
{
	return bShutdown.Load();
}
