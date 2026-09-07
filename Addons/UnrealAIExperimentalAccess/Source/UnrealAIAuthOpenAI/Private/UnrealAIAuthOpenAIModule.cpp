// Copyright EngineWorks. All Rights Reserved.

#include "OpenAI/UnrealAIAuthOpenAIModule.h"

#include "UnrealAIAuth.h"
#include "Auth/UnrealAIDeviceOAuthCompositionRoot.h"
#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "Auth/UnrealAIPlatformSecretStore.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"
#include "OpenAI/UnrealAIOpenAIDeviceOAuthDriver.h"
#include "Runtime/UnrealAIClock.h"
#include "Transport/UnrealAIOAuthHttpTransport.h"

namespace
{
const FName OpenAIModuleAuthProviderName(TEXT("openai.codex.oauth"));
const FName OpenAIModelProviderName(TEXT("openai.codex.responses"));
const FName OpenAIAuthProfile(TEXT("openai.codex.subscription"));
const FName OpenAIEndpointProfile(TEXT("openai.codex.responses"));
const FName OpenAIConnectionAlias(TEXT("openai.codex.subscription.default"));
const FName OpenAIAccountSelectionAlias(TEXT("openai.codex.account.default"));
const FUnrealAIAccessAccountId OpenAIAccountId{FGuid(0x621f7391, 0xa65043ca, 0x8110d75e, 0xa4361eb5)};
const FGuid OpenAISecretRecordId(0xaaf23a16, 0x2ca44e80, 0xabd62e2f, 0x8f5844c1);

constexpr bool IsExperimentalCompatibilityAllowed()
{
#if UE_BUILD_SHIPPING || UE_SERVER
	return false;
#else
	return true;
#endif
}

bool ComposeOpenAIConnections(
	const FUnrealAIProviderEndpointAuthority &Authority,
	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections, FString &OutError)
{
	OutConnections.Reset();
	FUnrealAIEndpointOrigin ResourceOrigin;
	FUnrealAIEndpointProfileRegistry EndpointRegistry;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> RegisteredEndpoint;
	if (!FUnrealAIEndpointOrigin::TryParse(
			TEXT("https://chatgpt.com"), false, ResourceOrigin, OutError) ||
			!EndpointRegistry.RegisterProviderSubscriptionEndpoint(
				Authority, OpenAIEndpointProfile, OpenAIModelProviderName, ResourceOrigin,
				TEXT("https://chatgpt.com/backend-api/codex/responses"),
					 static_cast<uint64>(FUnrealAIOpenAIDeviceOAuthDriver::GetCompatibilityRevision()),
					 RegisteredEndpoint, OutError))
	{
		return false;
	}

	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionAlias = OpenAIConnectionAlias;
	Connection.EndpointProfileId = OpenAIEndpointProfile;
	Connection.CredentialDestination.ModelProviderName = OpenAIModelProviderName;
	Connection.CredentialDestination.AccountAuthProviderName = OpenAIModuleAuthProviderName;
	Connection.CredentialDestination.AuthProfileId = OpenAIAuthProfile;
	Connection.CredentialDestination.AccountId = OpenAIAccountId;
	Connection.CredentialDestination.TenantRealm = TEXT("openai.chatgpt");
	Connection.CredentialDestination.BillingPrincipalId.Value = FGuid(0x27c13fc2, 0x57264abc, 0xaa1835f4, 0x06aaf209);
	Connection.CredentialDestination.PayerHandle = TEXT("openai.chatgpt.subscription");
	Connection.CredentialDestination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Connection.CredentialDestination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Connection.CredentialDestination.EndpointOrigin = ResourceOrigin;
	Connection.CredentialDestination.Audience = TEXT("https://chatgpt.com/backend-api/codex/responses");
	Connection.CredentialDestination.ConnectionRevision = 1;
	Connection.CredentialDestination.EndpointPolicyRevision =
		FUnrealAIOpenAIDeviceOAuthDriver::GetCompatibilityRevision();
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

class FUnrealAIAuthOpenAIModule final : public IUnrealAIAuthOpenAIModule
{
  public:
	void StartupModule() override
	{
		EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FUnrealAIAuthOpenAIModule::OnEnginePreExit);
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
		const TSharedPtr<FUnrealAIOpenAIDeviceOAuthDriver, ESPMode::ThreadSafe> NewDriver =
			MakeShared<FUnrealAIOpenAIDeviceOAuthDriver, ESPMode::ThreadSafe>(NewOAuthTransport.ToSharedRef(),
																			  NewClock.ToSharedRef());

		FUnrealAIDeviceOAuthCompositionConfig Config;
		Config.AccountProvider.ProviderName = OpenAIModuleAuthProviderName;
		Config.AccountProvider.ModelProviderName = OpenAIModelProviderName;
		Config.AccountProvider.AuthProfileId = OpenAIAuthProfile;
		Config.AccountProvider.AccountId = OpenAIAccountId;
		Config.AccountProvider.SecretHandle = FUnrealAISecretHandle{SecretStoreName, OpenAISecretRecordId};
		Config.AccountProvider.bRequiresProtectedSecondary = true;
		Config.AccountCatalog.SelectionAlias = OpenAIAccountSelectionAlias;
		Config.AccountCatalog.DisplayLabel = TEXT("OpenAI ChatGPT / Codex compatibility");
		Config.AccountCatalog.ProviderName = OpenAIModuleAuthProviderName;
		Config.AccountCatalog.AuthProfileId = OpenAIAuthProfile;
		Config.AccountCatalog.AccountId = OpenAIAccountId;
		Config.AccountCatalog.ConnectionAlias = OpenAIConnectionAlias;
		Config.bStartStoredSessionRecovery = !FApp::IsUnattended();

		IUnrealAIAuthModule &AuthModule = IUnrealAIAuthModule::Get();
		TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> NewRoot;
		FUnrealAIProviderEndpointAuthority Authority;
		FUnrealAIProviderAccessError SetupError;
		if (!FUnrealAIDeviceOAuthCompositionRoot::TryCreateAndRegister(
				Config, AuthModule.GetAccountAuthProviderRegistry(), AuthModule.GetAccountCatalogRegistry(),
				ComposeOpenAIConnections, NewDriver.ToSharedRef(), NewClock.ToSharedRef(), NewRoot, Authority,
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
		return OpenAIConnectionAlias;
	}

	FName GetAuthProfileId() const override
	{
		return OpenAIAuthProfile;
	}

	FUnrealAIAccessAccountId GetAccountId() const override
	{
		return OpenAIAccountId;
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
		Descriptor.ModelProviderName = OpenAIModelProviderName;
		Descriptor.AccountAuthProviderName = OpenAIModuleAuthProviderName;
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
			return AccountProviderPin->GetStatus(OpenAIAuthProfile, OpenAIAccountId);
		}
		FUnrealAIAccountStatus Status;
		Status.ProviderName = OpenAIModuleAuthProviderName;
		Status.AuthProfileId = OpenAIAuthProfile;
		Status.AccountId = OpenAIAccountId;
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
	bool StartSignInFromEditorGesture(const FUnrealAIOpenAIEditorAuthGesture &,
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

	bool StartSignOutFromEditorGesture(const FUnrealAIOpenAIEditorAuthGesture &,
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
		TSharedPtr<FUnrealAIOpenAIDeviceOAuthDriver, ESPMode::ThreadSafe> DriverToRelease;
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
	TSharedPtr<FUnrealAIOpenAIDeviceOAuthDriver, ESPMode::ThreadSafe> Driver;
	TSharedPtr<FUnrealAIDeviceOAuthCompositionRoot, ESPMode::ThreadSafe> Root;
	TSharedPtr<FUnrealAIDeviceOAuthAccountProvider, ESPMode::ThreadSafe> AccountProvider;
	TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker;
	mutable FCriticalSection LifecycleMutex;
	FDelegateHandle EnginePreExitHandle;
	bool bReady = false;
};

IMPLEMENT_MODULE(FUnrealAIAuthOpenAIModule, UnrealAIAuthOpenAI)
