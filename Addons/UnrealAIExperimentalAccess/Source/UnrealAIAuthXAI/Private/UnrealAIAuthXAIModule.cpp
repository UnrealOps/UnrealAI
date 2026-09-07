// Copyright EngineWorks. All Rights Reserved.

#include "XAI/UnrealAIAuthXAIModule.h"

#include "UnrealAIAuth.h"
#include "Auth/UnrealAIDeviceOAuthCompositionRoot.h"
#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "Auth/UnrealAIPlatformSecretStore.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"
#include "Runtime/UnrealAIClock.h"
#include "Transport/UnrealAIOAuthHttpTransport.h"
#include "XAI/UnrealAIXAIDeviceOAuthDriver.h"

namespace
{
const FName XAIModuleAuthProviderName(TEXT("xai.grok.oauth"));
const FName XAIModelProviderName(TEXT("xai.responses"));
const FName XAIAuthProfile(TEXT("xai.grok.subscription"));
const FName XAIEndpointProfile(TEXT("xai.responses"));
const FName XAIConnectionAlias(TEXT("xai.grok.subscription.default"));
const FName XAIAccountSelectionAlias(TEXT("xai.grok.account.default"));
const FUnrealAIAccessAccountId XAIAccountId{FGuid(0x6b10b3c8, 0x181246d8, 0xb30f187b, 0x56e87083)};
const FGuid XAISecretRecordId(0x74600586, 0xdc20426a, 0x822ec78f, 0x64418ec6);

constexpr bool IsExperimentalCompatibilityAllowed()
{
#if UE_BUILD_SHIPPING || UE_SERVER
	return false;
#else
	return true;
#endif
}

bool ComposeXAIConnections(const FUnrealAIProviderEndpointAuthority &Authority,
						   TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections,
						   FString &OutError)
{
	OutConnections.Reset();
	FUnrealAIEndpointOrigin ResourceOrigin;
	FUnrealAIEndpointProfileRegistry EndpointRegistry;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> RegisteredEndpoint;
	if (!FUnrealAIEndpointOrigin::TryParse(
			TEXT("https://api.x.ai"), false, ResourceOrigin, OutError) ||
			!EndpointRegistry.RegisterProviderSubscriptionEndpoint(
				Authority, XAIEndpointProfile, XAIModelProviderName, ResourceOrigin,
				TEXT("https://api.x.ai/v1/responses"),
					 static_cast<uint64>(FUnrealAIXAIDeviceOAuthDriver::GetCompatibilityRevision()), RegisteredEndpoint,
					 OutError))
	{
		return false;
	}

	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionAlias = XAIConnectionAlias;
	Connection.EndpointProfileId = XAIEndpointProfile;
	Connection.CredentialDestination.ModelProviderName = XAIModelProviderName;
	Connection.CredentialDestination.AccountAuthProviderName = XAIModuleAuthProviderName;
	Connection.CredentialDestination.AuthProfileId = XAIAuthProfile;
	Connection.CredentialDestination.AccountId = XAIAccountId;
	Connection.CredentialDestination.TenantRealm = TEXT("xai.grok");
	Connection.CredentialDestination.BillingPrincipalId.Value = FGuid(0x727d7764, 0x57be4eb1, 0xbc09a1d9, 0xcd72c44c);
	Connection.CredentialDestination.PayerHandle = TEXT("xai.grok.subscription");
	Connection.CredentialDestination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Connection.CredentialDestination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Connection.CredentialDestination.EndpointOrigin = ResourceOrigin;
	Connection.CredentialDestination.Audience = TEXT("https://api.x.ai/v1/responses");
	Connection.CredentialDestination.ConnectionRevision = 1;
	Connection.CredentialDestination.EndpointPolicyRevision = FUnrealAIXAIDeviceOAuthDriver::GetCompatibilityRevision();
	FUnrealAIConnectionRegistry ConnectionRegistry(EndpointRegistry.CreateSnapshot());
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> RegisteredConnection;
	if (!ConnectionRegistry.Register(Connection, RegisteredConnection, OutError))
	{
		return false;
	}
	OutConnections = ConnectionRegistry.CreateSnapshot();
	return true;
}
} // namespace

class FUnrealAIAuthXAIModule final : public IUnrealAIAuthXAIModule
{
  public:
	void StartupModule() override
	{
		EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FUnrealAIAuthXAIModule::OnEnginePreExit);
		if (!IsExperimentalCompatibilityAllowed() || !IsAgentPlatformOAuthHttpsTransportSupported())
		{
			return;
		}
		const FUnrealAIPlatformSecretStoreSelection StoreSelection =
			FUnrealAIPlatformSecretStoreSelection::CurrentUserOAuth(false);
		const FName SecretStoreName = FUnrealAIPlatformSecretStore::GetSelectedStoreName(StoreSelection);
		if (SecretStoreName.IsNone())
		{
			return;
		}

		const TSharedPtr<FUnrealAISystemClock, ESPMode::ThreadSafe> NewClock =
			MakeShared<FUnrealAISystemClock, ESPMode::ThreadSafe>();
		const TSharedPtr<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> NewOAuthTransport =
			CreateAgentPlatformOAuthHttpsTransport();
		const TSharedPtr<FUnrealAIXAIDeviceOAuthDriver, ESPMode::ThreadSafe> NewDriver =
			MakeShared<FUnrealAIXAIDeviceOAuthDriver, ESPMode::ThreadSafe>(NewOAuthTransport.ToSharedRef(),
																		   NewClock.ToSharedRef());

		FUnrealAIDeviceOAuthCompositionConfig Config;
		Config.AccountProvider.ProviderName = XAIModuleAuthProviderName;
		Config.AccountProvider.ModelProviderName = XAIModelProviderName;
		Config.AccountProvider.AuthProfileId = XAIAuthProfile;
		Config.AccountProvider.AccountId = XAIAccountId;
		Config.AccountProvider.SecretHandle = FUnrealAISecretHandle{SecretStoreName, XAISecretRecordId};
		Config.AccountProvider.bRequiresProtectedSecondary = false;
		Config.AccountCatalog.SelectionAlias = XAIAccountSelectionAlias;
		Config.AccountCatalog.DisplayLabel = TEXT("xAI Grok compatibility");
		Config.AccountCatalog.ProviderName = XAIModuleAuthProviderName;
		Config.AccountCatalog.AuthProfileId = XAIAuthProfile;
		Config.AccountCatalog.AccountId = XAIAccountId;
		Config.AccountCatalog.ConnectionAlias = XAIConnectionAlias;
		Config.bStartStoredSessionRecovery = !FApp::IsUnattended();

		IUnrealAIAuthModule &AuthModule = IUnrealAIAuthModule::Get();
		TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> NewRoot;
		FUnrealAIProviderEndpointAuthority Authority;
		FUnrealAIProviderAccessError SetupError;
		if (!FUnrealAIDeviceOAuthCompositionRoot::TryCreateAndRegister(
				Config, AuthModule.GetAccountAuthProviderRegistry(), AuthModule.GetAccountCatalogRegistry(),
				ComposeXAIConnections, NewDriver.ToSharedRef(), NewClock.ToSharedRef(), NewRoot, Authority,
				SetupError) ||
			!NewRoot.IsValid())
		{
			NewOAuthTransport->BeginShutdown();
			return;
		}

		FScopeLock LifecycleLock(&LifecycleMutex);
		Clock = NewClock;
		OAuthTransport = NewOAuthTransport;
		Driver = NewDriver;
		Root = NewRoot;
		AccountProvider = NewRoot->GetAccountProvider();
		Broker = NewRoot->GetCredentialBroker();
		Connections = NewRoot->GetConnections();
		bReady = true;
	}

	void ShutdownModule() override
	{
		if (EnginePreExitHandle.IsValid())
		{
			FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);
			EnginePreExitHandle.Reset();
		}
		ResetState();
		UE_LOG(LogTemp, Fatal, TEXT("Explicit unload of native subscription authentication is unsupported."));
	}

	bool SupportsDynamicReloading() override
	{
		return false;
	}

	bool SupportsAutomaticShutdown() override
	{
		return false;
	}

	bool IsRuntimeReady() const override
	{
		FScopeLock LifecycleLock(&LifecycleMutex);
		return IsRuntimeReadyLocked();
	}

	FName GetConnectionAlias() const override
	{
		return XAIConnectionAlias;
	}

	FName GetAuthProfileId() const override
	{
		return XAIAuthProfile;
	}

	FUnrealAIAccessAccountId GetAccountId() const override
	{
		return XAIAccountId;
	}

	FUnrealAIProviderAccessDescriptor DescribeSubscriptionAccess() const override
	{
		TSharedPtr<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> AccountProviderPin;
		{
			FScopeLock LifecycleLock(&LifecycleMutex);
			if (IsRuntimeReadyLocked())
			{
				AccountProviderPin = AccountProvider;
			}
		}
		if (AccountProviderPin.IsValid())
		{
			FUnrealAIProviderAccessDescriptor Descriptor = AccountProviderPin->DescribeAccess();
			Descriptor.SupportClassification =
				EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
			return Descriptor;
		}
		FUnrealAIProviderAccessDescriptor Descriptor;
		Descriptor.ModelProviderName = XAIModelProviderName;
		Descriptor.AccountAuthProviderName = XAIModuleAuthProviderName;
		Descriptor.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
		Descriptor.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
		Descriptor.Availability = IsExperimentalCompatibilityAllowed()
									  ? EUnrealAIProviderAccessAvailability::Unavailable
									  : EUnrealAIProviderAccessAvailability::Unsupported;
		Descriptor.SupportClassification =
			EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
		return Descriptor;
	}

	FUnrealAIAccountStatus GetAccountStatus() const override
	{
		TSharedPtr<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> AccountProviderPin;
		{
			FScopeLock LifecycleLock(&LifecycleMutex);
			if (IsRuntimeReadyLocked())
			{
				AccountProviderPin = AccountProvider;
			}
		}
		if (AccountProviderPin.IsValid())
		{
			return AccountProviderPin->GetStatus(XAIAuthProfile, XAIAccountId);
		}
		FUnrealAIAccountStatus Status;
		Status.ProviderName = XAIModuleAuthProviderName;
		Status.AuthProfileId = XAIAuthProfile;
		Status.AccountId = XAIAccountId;
		Status.State = EUnrealAIAccountAuthState::Invalid;
		return Status;
	}

	TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> GetCredentialBroker() const override
	{
		FScopeLock LifecycleLock(&LifecycleMutex);
		return IsRuntimeReadyLocked() ? Broker : nullptr;
	}

	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> GetConnections() const override
	{
		FScopeLock LifecycleLock(&LifecycleMutex);
		return IsRuntimeReadyLocked() ? Connections : nullptr;
	}

#if WITH_EDITOR
	bool StartSignInFromEditorGesture(const FUnrealAIXAIEditorAuthGesture &,
									  const FUnrealAIInteractiveAuthRequest &Request,
									  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
									  const FUnrealAICancellationToken &Cancellation,
									  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
									  FUnrealAIProviderAccessError &OutError) override
	{
		TSharedPtr<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> AccountProviderPin;
		{
			FScopeLock LifecycleLock(&LifecycleMutex);
			if (IsRuntimeReadyLocked())
			{
				AccountProviderPin = AccountProvider;
			}
		}
		if (!AccountProviderPin.IsValid())
		{
			OutHandle.Reset();
			OutError.Category = EUnrealAIErrorCategory::InvalidConfiguration;
			OutError.Code = EUnrealAIProviderAccessErrorCode::InvalidConfiguration;
			return false;
		}
		const FUnrealAITrustedLocalAuthGesture TrustedGesture;
		return AccountProviderPin->StartSignIn(TrustedGesture, Request, MoveTemp(Sink), Cancellation, OutHandle,
											   OutError);
	}

	bool StartSignOutFromEditorGesture(const FUnrealAIXAIEditorAuthGesture &,
									   const FUnrealAIAccountAuthRequest &Request,
									   TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
									   const FUnrealAICancellationToken &Cancellation,
									   TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
									   FUnrealAIProviderAccessError &OutError) override
	{
		TSharedPtr<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> AccountProviderPin;
		{
			FScopeLock LifecycleLock(&LifecycleMutex);
			if (IsRuntimeReadyLocked())
			{
				AccountProviderPin = AccountProvider;
			}
		}
		if (!AccountProviderPin.IsValid())
		{
			OutHandle.Reset();
			OutError.Category = EUnrealAIErrorCategory::InvalidConfiguration;
			OutError.Code = EUnrealAIProviderAccessErrorCode::InvalidConfiguration;
			return false;
		}
		const FUnrealAITrustedLocalAuthGesture TrustedGesture;
		return AccountProviderPin->StartSignOut(TrustedGesture, Request, MoveTemp(Sink), Cancellation, OutHandle,
												OutError);
	}
#endif

  private:
	bool IsRuntimeReadyLocked() const
	{
		return bReady && Root.IsValid() && !Root->IsShutdown() && AccountProvider.IsValid() && Broker.IsValid() &&
			   Connections.IsValid();
	}

	void OnEnginePreExit()
	{
		TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> RootPin;
		TSharedPtr<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> OAuthTransportPin;
		{
			FScopeLock LifecycleLock(&LifecycleMutex);
			bReady = false;
			RootPin = Root;
			OAuthTransportPin = OAuthTransport;
		}
		if (RootPin.IsValid())
		{
			RootPin->BeginShutdown();
		}
		if (OAuthTransportPin.IsValid())
		{
			OAuthTransportPin->BeginShutdown();
		}
	}

	void ResetState()
	{
		TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> RootToRelease;
		TSharedPtr<FUnrealAIXAIDeviceOAuthDriver, ESPMode::ThreadSafe> DriverToRelease;
		TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> ConnectionsToRelease;
		TSharedPtr<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> OAuthTransportToRelease;
		TSharedPtr<FUnrealAISystemClock, ESPMode::ThreadSafe> ClockToRelease;
		{
			FScopeLock LifecycleLock(&LifecycleMutex);
			bReady = false;
			RootToRelease = MoveTemp(Root);
			Broker.Reset();
			AccountProvider.Reset();
			DriverToRelease = MoveTemp(Driver);
			ConnectionsToRelease = MoveTemp(Connections);
			OAuthTransportToRelease = MoveTemp(OAuthTransport);
			ClockToRelease = MoveTemp(Clock);
		}
		if (RootToRelease.IsValid())
		{
			RootToRelease->BeginShutdown();
		}
		if (OAuthTransportToRelease.IsValid())
		{
			OAuthTransportToRelease->BeginShutdown();
		}
		RootToRelease.Reset();
		DriverToRelease.Reset();
		ConnectionsToRelease.Reset();
		OAuthTransportToRelease.Reset();
		ClockToRelease.Reset();
	}

	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	TSharedPtr<FUnrealAISystemClock, ESPMode::ThreadSafe> Clock;
	TSharedPtr<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> OAuthTransport;
	TSharedPtr<FUnrealAIXAIDeviceOAuthDriver, ESPMode::ThreadSafe> Driver;
	TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> Root;
	TSharedPtr<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> AccountProvider;
	TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker;
	mutable FCriticalSection LifecycleMutex;
	FDelegateHandle EnginePreExitHandle;
	bool bReady = false;
};

IMPLEMENT_MODULE(FUnrealAIAuthXAIModule, UnrealAIAuthXAI)
