// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIDeviceOAuthCompositionRoot.h"

#include "Auth/UnrealAIPlatformSecretStore.h"
#include "Runtime/UnrealAIClock.h"

namespace
{
FUnrealAIProviderAccessError MakeDeviceCompositionError(const EUnrealAIErrorCategory Category,
														const EUnrealAIProviderAccessErrorCode Code)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	return Error;
}

bool HasExactDeviceOAuthConnection(const FUnrealAIConnectionRegistrySnapshot &Connections,
								   const FUnrealAIDeviceOAuthAccountProviderConfig &Config,
								   const FUnrealAIAccountCatalogEntry &CatalogEntry)
{
	const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection =
		Connections.Find(CatalogEntry.ConnectionAlias);
	if (!Connection.IsValid())
	{
		return false;
	}
	const FUnrealAICredentialDestination &Destination = Connection->CredentialDestination;
	return Destination.ModelProviderName == Config.ModelProviderName &&
		   Destination.AccountAuthProviderName == Config.ProviderName &&
		   Destination.AuthProfileId == Config.AuthProfileId && Destination.AccountId == Config.AccountId &&
		   Destination.AuthScheme == EUnrealAIAuthScheme::OAuthBearer &&
		   Destination.BillingMode == EUnrealAIBillingMode::SubscriptionQuota;
}
} // namespace

bool FUnrealAIDeviceOAuthCompositionConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString NestedError;
	if (!AccountProvider.ValidateShape(NestedError) || !CredentialBroker.ValidateShape(NestedError) ||
		!AccountCatalog.ValidateShape(NestedError) || AccountCatalog.ProviderName != AccountProvider.ProviderName ||
		AccountCatalog.AuthProfileId != AccountProvider.AuthProfileId ||
		!(AccountCatalog.AccountId == AccountProvider.AccountId))
	{
		OutError = TEXT("Device OAuth composition configuration is invalid.");
		return false;
	}
	return true;
}

FUnrealAIDeviceOAuthCompositionRoot::FUnrealAIDeviceOAuthCompositionRoot(
	FUnrealAIAccountAuthProviderRegistry &InProviderRegistry, FUnrealAIAccountCatalogRegistry &InAccountCatalog,
	const FName InSelectionAlias, TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
	TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InCredentialBroker,
	TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> InAccountProvider)
	: ProviderRegistry(&InProviderRegistry), AccountCatalog(&InAccountCatalog), SelectionAlias(InSelectionAlias),
	  Clock(MoveTemp(InClock)), SecretStore(MoveTemp(InSecretStore)), Connections(MoveTemp(InConnections)),
	  CredentialBroker(MoveTemp(InCredentialBroker)), AccountProvider(MoveTemp(InAccountProvider))
{
}

FUnrealAIDeviceOAuthCompositionRoot::~FUnrealAIDeviceOAuthCompositionRoot()
{
	BeginShutdown();
}

bool FUnrealAIDeviceOAuthCompositionRoot::TryCreateAndRegister(
	const FUnrealAIDeviceOAuthCompositionConfig &Config, FUnrealAIAccountAuthProviderRegistry &ProviderRegistry,
	FUnrealAIAccountCatalogRegistry &AccountCatalog, FUnrealAIDeviceOAuthConnectionComposer ComposeConnections,
	TSharedRef<IUnrealAIDeviceOAuthAuthorizationDriver, ESPMode::ThreadSafe> Driver,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
	TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> &OutRoot,
	FUnrealAIProviderEndpointAuthority &OutAuthority, FUnrealAIProviderAccessError &OutError)
{
	OutRoot.Reset();
	OutAuthority = {};
	OutError = {};
	FString ShapeError;
	if (!Config.ValidateShape(ShapeError))
	{
		OutError = MakeDeviceCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
											  EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	FUnrealAISecretStoreCapabilities StoreCapabilities;
	if (!FUnrealAIPlatformSecretStore::TryCreate(
			FUnrealAIPlatformSecretStoreSelection::CurrentUserOAuth(UE_BUILD_SHIPPING != 0), SecretStore,
			StoreCapabilities, OutError) ||
		!SecretStore.IsValid())
	{
		return false;
	}
	return TryCreateAndRegisterWithStore(Config, ProviderRegistry, AccountCatalog, ComposeConnections, MoveTemp(Driver),
										 SecretStore.ToSharedRef(), Clock, OutRoot, OutAuthority, OutError);
}

bool FUnrealAIDeviceOAuthCompositionRoot::TryCreateAndRegisterWithStore(
	const FUnrealAIDeviceOAuthCompositionConfig &Config, FUnrealAIAccountAuthProviderRegistry &ProviderRegistry,
	FUnrealAIAccountCatalogRegistry &AccountCatalog, FUnrealAIDeviceOAuthConnectionComposer ComposeConnections,
	TSharedRef<IUnrealAIDeviceOAuthAuthorizationDriver, ESPMode::ThreadSafe> Driver,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
	TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> &OutRoot,
	FUnrealAIProviderEndpointAuthority &OutAuthority, FUnrealAIProviderAccessError &OutError)
{
	OutRoot.Reset();
	OutAuthority = {};
	OutError = {};
	FString SetupError;
	if (!Config.ValidateShape(SetupError) || Driver->GetProviderName() != Config.AccountProvider.ProviderName ||
		Config.AccountProvider.SecretHandle.StoreName != SecretStore->GetStoreName())
	{
		OutError = MakeDeviceCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
											  EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	const TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>(Config.AccountProvider, SecretStore, Clock,
																			 Driver);
	if (!ProviderRegistry.Register(Provider, OutAuthority, SetupError) || !OutAuthority.IsValid())
	{
		Provider->BeginShutdown();
		OutAuthority = {};
		OutError = MakeDeviceCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
											  EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	if (!ComposeConnections(OutAuthority, Connections, SetupError) || !Connections.IsValid() ||
		!HasExactDeviceOAuthConnection(*Connections, Config.AccountProvider, Config.AccountCatalog))
	{
		FString RollbackError;
		ProviderRegistry.Unregister(Provider, RollbackError);
		Provider->BeginShutdown();
		OutAuthority = {};
		OutError = MakeDeviceCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
											  EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	const TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker =
		MakeShared<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe>(Connections.ToSharedRef(), SecretStore, Clock,
																		Driver, Config.CredentialBroker);
	FUnrealAIOAuthCredentialBinding Binding;
	Binding.ProviderName = Config.AccountProvider.ProviderName;
	Binding.AuthProfileId = Config.AccountProvider.AuthProfileId;
	Binding.AccountId = Config.AccountProvider.AccountId;
	Binding.SecretHandle = Config.AccountProvider.SecretHandle;
	Binding.bRequiresProtectedSecondary = Config.AccountProvider.bRequiresProtectedSecondary;
	if (!Broker->RegisterBinding(Binding, SetupError) || !Provider->SetCredentialBroker(Broker, SetupError))
	{
		FString RollbackError;
		ProviderRegistry.Unregister(Provider, RollbackError);
		Provider->BeginShutdown();
		Broker->BeginShutdown();
		OutAuthority = {};
		OutError = MakeDeviceCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
											  EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> CatalogConnection =
		Connections->Find(Config.AccountCatalog.ConnectionAlias);
	if (!CatalogConnection.IsValid() ||
		!AccountCatalog.Register(Config.AccountCatalog, Provider, CatalogConnection.ToSharedRef(), SetupError))
	{
		FString RollbackError;
		ProviderRegistry.Unregister(Provider, RollbackError);
		Provider->BeginShutdown();
		Broker->BeginShutdown();
		OutAuthority = {};
		OutError = MakeDeviceCompositionError(EUnrealAIErrorCategory::InvalidConfiguration,
											  EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}

	OutRoot = MakeShareable(
		new FUnrealAIDeviceOAuthCompositionRoot(ProviderRegistry, AccountCatalog, Config.AccountCatalog.SelectionAlias,
												Clock, SecretStore, Connections.ToSharedRef(), Broker, Provider));
	if (Config.bStartStoredSessionRecovery)
	{
		Provider->StartStoredSessionRecovery();
	}
	return true;
}

TSharedRef<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe>
FUnrealAIDeviceOAuthCompositionRoot::GetAccountProvider() const
{
	return AccountProvider;
}

TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe>
FUnrealAIDeviceOAuthCompositionRoot::GetCredentialBroker() const
{
	return CredentialBroker;
}

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe>
FUnrealAIDeviceOAuthCompositionRoot::GetConnections() const
{
	return Connections;
}

TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> FUnrealAIDeviceOAuthCompositionRoot::GetSecretStore() const
{
	return SecretStore;
}

void FUnrealAIDeviceOAuthCompositionRoot::BeginShutdown()
{
	if (bShutdown.Exchange(true))
	{
		return;
	}
	FString Error;
	if (AccountCatalog != nullptr)
	{
		AccountCatalog->Unregister(SelectionAlias, AccountProvider, Error);
		AccountCatalog = nullptr;
	}
	if (ProviderRegistry != nullptr)
	{
		ProviderRegistry->Unregister(AccountProvider, Error);
		ProviderRegistry = nullptr;
	}
	AccountProvider->BeginShutdown();
	CredentialBroker->BeginShutdown();
}

bool FUnrealAIDeviceOAuthCompositionRoot::IsShutdown() const
{
	return bShutdown.Load();
}
