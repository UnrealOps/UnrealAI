// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"

#include "Auth/UnrealAIBrowserOAuthCompositionRoot.h"
#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "Auth/UnrealAIMemorySecretStore.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

class FUnrealAIBrowserOAuthAccountProviderTestGestureAuthority final
{
  public:
	static bool StartSignIn(FUnrealAIBrowserOAuthAccountProvider &Provider,
							const FUnrealAIInteractiveAuthRequest &Request,
							TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
							const FUnrealAICancellationToken &Cancellation,
							TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							FUnrealAIProviderAccessError &OutError)
	{
		const FUnrealAITrustedLocalAuthGesture Gesture;
		return Provider.StartSignIn(Gesture, Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
	}

	static bool StartSignOut(FUnrealAIBrowserOAuthAccountProvider &Provider, const FUnrealAIAccountAuthRequest &Request,
							 TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
							 const FUnrealAICancellationToken &Cancellation,
							 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							 FUnrealAIProviderAccessError &OutError)
	{
		const FUnrealAITrustedLocalAuthGesture Gesture;
		return Provider.StartSignOut(Gesture, Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
	}
};

namespace
{
constexpr TCHAR BrowserStoreName[] = TEXT("tests.browser_oauth.store");
constexpr TCHAR BrowserProviderName[] = TEXT("tests.browser_oauth.auth");
constexpr TCHAR BrowserModelProviderName[] = TEXT("tests.browser_oauth.model");
constexpr TCHAR BrowserProfileName[] = TEXT("tests.browser_oauth.profile");
constexpr TCHAR BrowserEndpointName[] = TEXT("tests.browser_oauth.endpoint");
constexpr TCHAR BrowserConnectionName[] = TEXT("tests.browser_oauth.connection");
constexpr TCHAR BrowserSelectionName[] = TEXT("tests.browser_oauth.account.default");

const FUnrealAIAccessAccountId BrowserAccountId{FGuid(0x13572468, 0x24681357, 0xabcdef01, 0x23456789)};
const FUnrealAISecretHandle BrowserSecretHandle{BrowserStoreName,
												FGuid(0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f001)};

FUnrealAIProviderAccessError MakeBrowserTestError(const EUnrealAIErrorCategory Category,
												  const EUnrealAIProviderAccessErrorCode Code,
												  const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

bool MakeBrowserTestSecret(const ANSICHAR *Text, FUnrealAISecretValue &OutSecret)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Text), FCStringAnsi::Strlen(Text));
	FString Error;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, Error);
}

bool WaitForBrowserCondition(TFunctionRef<bool()> Predicate, const double TimeoutSeconds = 5.0)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!Predicate() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return Predicate();
}

FUnrealAIOAuthTrustedAuthorizationServer MakeBrowserServer()
{
	FUnrealAIOAuthTrustedAuthorizationServer Server;
	Server.Issuer = TEXT("https://issuer.browser.test");
	Server.DiscoveryEndpoint = TEXT("https://issuer.browser.test/.well-known/openid-configuration");
	Server.AuthorizationEndpoint = TEXT("https://issuer.browser.test/oauth2/authorize");
	Server.TokenEndpoint = TEXT("https://issuer.browser.test/oauth2/token");
	Server.JwksEndpoint = TEXT("https://issuer.browser.test/oauth2/jwks");
	Server.RevocationEndpoint = TEXT("https://issuer.browser.test/oauth2/revoke");
	Server.AllowedSigningAlgorithms = {FName(TEXT("RS256"))};
	return Server;
}

FUnrealAIBrowserOAuthCompositionConfig MakeBrowserConfig(const bool bStartRecovery = false)
{
	FUnrealAIBrowserOAuthCompositionConfig Config;
	Config.AccountProvider.ProviderName = BrowserProviderName;
	Config.AccountProvider.ModelProviderName = BrowserModelProviderName;
	Config.AccountProvider.AuthProfileId = BrowserProfileName;
	Config.AccountProvider.AccountId = BrowserAccountId;
	Config.AccountProvider.SecretHandle = BrowserSecretHandle;
	Config.AccountProvider.Authorization.Server = MakeBrowserServer();
	Config.AccountProvider.Authorization.ClientId = TEXT("browser-public-client");
	Config.AccountProvider.Authorization.Audience = TEXT("https://resource.browser.test/v1/responses");
	Config.AccountProvider.Authorization.ExactRedirectUri = TEXT("http://127.0.0.1:43831/callback");
	Config.AccountProvider.Authorization.RequestedScopes = {TEXT("openid"), TEXT("profile"), TEXT("offline_access")};
	Config.AccountProvider.CleanupTimeoutSeconds = 5.0;
	Config.AccountProvider.SupportClassification =
		EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
	Config.AccountCatalog.SelectionAlias = BrowserSelectionName;
	Config.AccountCatalog.DisplayLabel = TEXT("Browser OAuth test account");
	Config.AccountCatalog.ProviderName = BrowserProviderName;
	Config.AccountCatalog.AuthProfileId = BrowserProfileName;
	Config.AccountCatalog.AccountId = BrowserAccountId;
	Config.AccountCatalog.ConnectionAlias = BrowserConnectionName;
	Config.bStartStoredSessionRecovery = bStartRecovery;
	return Config;
}

FUnrealAIInteractiveAuthRequest MakeBrowserSignInRequest(const uint32 Seed)
{
	FUnrealAIInteractiveAuthRequest Request;
	Request.RequestId.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	Request.AuthProfileId = BrowserProfileName;
	Request.Flow = EUnrealAIInteractiveAuthFlow::BrowserPkce;
	Request.TimeoutSeconds = 30.0f;
	return Request;
}

FUnrealAIAccountAuthRequest MakeBrowserSignOutRequest(const uint32 Seed)
{
	FUnrealAIAccountAuthRequest Request;
	Request.RequestId.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	Request.AuthProfileId = BrowserProfileName;
	Request.AccountId = BrowserAccountId;
	Request.TimeoutSeconds = 30.0f;
	return Request;
}

class FBrowserTestStore final : public IUnrealAISecretStore
{
  public:
	FBrowserTestStore()
		: Inner(MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(BrowserStoreName, 8)),
		  LoadEntered(FPlatformProcess::GetSynchEventFromPool(true)),
		  ReleaseLoad(FPlatformProcess::GetSynchEventFromPool(true))
	{
		ReleaseLoad->Trigger();
	}

	~FBrowserTestStore() override
	{
		ReleaseLoad->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(LoadEntered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseLoad);
	}

	FName GetStoreName() const override
	{
		return BrowserStoreName;
	}

	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override
	{
		FUnrealAISecretStoreCapabilities Capabilities;
		Capabilities.PersistenceClass = EUnrealAISecretStorePersistenceClass::Persistent;
		Capabilities.ProtectionClass = EUnrealAISecretStoreProtectionClass::PlatformCredentialStore;
		Capabilities.ScopeClass = EUnrealAISecretStoreScopeClass::CurrentUser;
		Capabilities.bAvailableInCurrentBuild = true;
		Capabilities.bPersistent = true;
		Capabilities.bAtomicCompareAndSwap = true;
		Capabilities.bAvailableInShipping = false;
		return Capabilities;
	}

	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &Context,
									const FUnrealAISecretHandle &Handle, FUnrealAISecretValue &OutValue,
									uint64 &OutRevision, FUnrealAIProviderAccessError &OutError) override
	{
		++LoadCalls;
		if (bBlockNextLoad.Exchange(false))
		{
			LoadEntered->Trigger();
			ReleaseLoad->Wait(5000);
		}
		return Inner->Load(Context, Handle, OutValue, OutRevision, OutError);
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &Context,
									 const FUnrealAISecretHandle &Handle, const FUnrealAISecretValue &Value,
									 const uint64 ExpectedRevision, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		++StoreCalls;
		if (bFailNextStore.Exchange(false))
		{
			OutNewRevision = 0;
			OutError = MakeBrowserTestError(EUnrealAIErrorCategory::Persistence,
											EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, true);
			return EUnrealAISecretStoreResult::Unavailable;
		}
		return Inner->Store(Context, Handle, Value, ExpectedRevision, OutNewRevision, OutError);
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &Context,
									  const FUnrealAISecretHandle &Handle, const uint64 ExpectedRevision,
									  FUnrealAIProviderAccessError &OutError) override
	{
		++DeleteCalls;
		if (OnDelete)
		{
			OnDelete();
		}
		return Inner->Delete(Context, Handle, ExpectedRevision, OutError);
	}

	int32 Num() const
	{
		return Inner->Num();
	}

	void BlockNextLoad()
	{
		bBlockNextLoad.Store(true);
		LoadEntered->Reset();
		ReleaseLoad->Reset();
	}

	bool WaitForBlockedLoad() const
	{
		return LoadEntered->Wait(3000);
	}

	void ReleaseBlockedLoad()
	{
		ReleaseLoad->Trigger();
	}

	TAtomic<bool> bFailNextStore{false};
	TAtomic<int32> LoadCalls{0};
	TAtomic<int32> StoreCalls{0};
	TAtomic<int32> DeleteCalls{0};
	TFunction<void()> OnDelete;

  private:
	TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Inner;
	FEvent *LoadEntered = nullptr;
	FEvent *ReleaseLoad = nullptr;
	TAtomic<bool> bBlockNextLoad{false};
};

class FBrowserTestExecutor final : public IUnrealAIOAuthAuthorizationExecutor
{
  public:
	explicit FBrowserTestExecutor(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock)
		: Clock(MoveTemp(InClock)), AuthorizeEntered(FPlatformProcess::GetSynchEventFromPool(true)),
		  ReleaseAuthorize(FPlatformProcess::GetSynchEventFromPool(true))
	{
		ReleaseAuthorize->Trigger();
	}

	~FBrowserTestExecutor() override
	{
		ReleaseAuthorize->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(AuthorizeEntered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseAuthorize);
	}

	void BlockAuthorization(const bool bIgnoreCancellation = true)
	{
		bBlockAuthorize.Store(true);
		bIgnoreAuthorizeCancellation.Store(bIgnoreCancellation);
		AuthorizeEntered->Reset();
		ReleaseAuthorize->Reset();
	}

	void ReleaseAuthorization()
	{
		ReleaseAuthorize->Trigger();
	}

	bool WaitForAuthorization() const
	{
		return AuthorizeEntered->Wait(3000);
	}

	bool AuthorizeBrowserPkce(const FUnrealAIOAuthBrowserAuthorizationRequest &,
							  const FUnrealAICancellationToken &Cancellation,
							  FUnrealAIOAuthAuthorizationResult &OutResult,
							  FUnrealAIProviderAccessError &OutError) override
	{
		++AuthorizeCalls;
		AuthorizeEntered->Trigger();
		if (bBlockAuthorize.Load())
		{
			ReleaseAuthorize->Wait(5000);
		}
		if (Cancellation.IsCancellationRequested() && !bIgnoreAuthorizeCancellation.Load())
		{
			OutError = MakeBrowserTestError(EUnrealAIErrorCategory::Cancelled,
											EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}
		if (!MakeBrowserTestSecret("browser-access-token", OutResult.Tokens.AccessToken) ||
			!MakeBrowserTestSecret("browser-refresh-token", OutResult.Tokens.RefreshToken))
		{
			OutResult.Reset();
			OutError = MakeBrowserTestError(EUnrealAIErrorCategory::Memory,
											EUnrealAIProviderAccessErrorCode::SecretCopyFailed);
			return false;
		}
		OutResult.Tokens.AccessTokenExpiresAtUtc = Clock->UtcNow() + FTimespan::FromHours(1);
		OutResult.SubjectFingerprint = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
		OutResult.GrantedScopes = {TEXT("openid"), TEXT("profile"), TEXT("offline_access")};
		OutError = {};
		return true;
	}

	bool Revoke(const FUnrealAIOAuthRevocationRequest &Request, FUnrealAISecretValue &&Token,
				const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) override
	{
		++RevokeCalls;
		bRevocationHadToken.Store(Token.IsSet());
		bRevocationUsedFreshCancellation.Store(!Cancellation.IsCancellationRequested());
		bRevocationUsedExactAuthority.Store(Request.Server.RevocationEndpoint ==
											TEXT("https://issuer.browser.test/oauth2/revoke") &&
												 Request.ClientId == TEXT("browser-public-client"));
		Token.Reset();
		if (bFailRevocation.Load())
		{
			OutError = MakeBrowserTestError(EUnrealAIErrorCategory::Provider,
											EUnrealAIProviderAccessErrorCode::AuthFailed, true);
			return false;
		}
		bRevocationCompleted.Store(true);
		OutError = {};
		return true;
	}

	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TAtomic<int32> AuthorizeCalls{0};
	TAtomic<int32> RevokeCalls{0};
	TAtomic<bool> bRevocationHadToken{false};
	TAtomic<bool> bRevocationUsedFreshCancellation{false};
	TAtomic<bool> bRevocationUsedExactAuthority{false};
	TAtomic<bool> bRevocationCompleted{false};
	TAtomic<bool> bFailRevocation{false};

  private:
	FEvent *AuthorizeEntered = nullptr;
	FEvent *ReleaseAuthorize = nullptr;
	TAtomic<bool> bBlockAuthorize{false};
	TAtomic<bool> bIgnoreAuthorizeCancellation{true};
};

class FBrowserTestRefreshSource final : public IUnrealAIOAuthCredentialRefreshSource
{
  public:
	FName GetProviderName() const override
	{
		return BrowserProviderName;
	}

	bool Refresh(FUnrealAIOAuthTokenEnvelope &, double, const FUnrealAICancellationToken &,
				 FUnrealAIProviderAccessError &OutError) override
	{
		++RefreshCalls;
		OutError =
			MakeBrowserTestError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed);
		return false;
	}

	TAtomic<int32> RefreshCalls{0};
};

class FBrowserAuthSink final : public IUnrealAIAuthEventSink
{
  public:
	void EnqueueAuthEvent(FUnrealAIAuthEvent &&Event) override
	{
		FScopeLock Lock(&Mutex);
		Events.Add(MoveTemp(Event));
	}

	bool WaitForTerminal(const double TimeoutSeconds = 5.0) const
	{
		return WaitForBrowserCondition(
			[this]()
			{
				FScopeLock Lock(&Mutex);
				return Events.ContainsByPredicate([](const FUnrealAIAuthEvent &Event) { return Event.IsTerminal(); });
			},
			TimeoutSeconds);
	}

	EUnrealAIAuthEventKind GetTerminalKind() const
	{
		FScopeLock Lock(&Mutex);
		for (const FUnrealAIAuthEvent &Event : Events)
		{
			if (Event.IsTerminal())
			{
				return Event.Kind;
			}
		}
		return EUnrealAIAuthEventKind::Invalid;
	}

	int32 NumTerminals() const
	{
		FScopeLock Lock(&Mutex);
		int32 Count = 0;
		for (const FUnrealAIAuthEvent &Event : Events)
		{
			Count += Event.IsTerminal() ? 1 : 0;
		}
		return Count;
	}

  private:
	mutable FCriticalSection Mutex;
	TArray<FUnrealAIAuthEvent> Events;
};

class FBrowserCredentialSink final : public IUnrealAICredentialResultSink
{
  public:
	void EnqueueCredentialResult(FUnrealAICredentialResult &&Result) override
	{
		FScopeLock Lock(&Mutex);
		Terminal = MakeUnique<FUnrealAICredentialResult>(MoveTemp(Result));
	}

	bool WaitAndTake(FUnrealAICredentialResult &OutResult)
	{
		if (!WaitForBrowserCondition(
				[this]()
				{
					FScopeLock Lock(&Mutex);
					return Terminal.IsValid();
				}))
		{
			return false;
		}
		FScopeLock Lock(&Mutex);
		OutResult = MoveTemp(*Terminal);
		Terminal.Reset();
		return true;
	}

  private:
	FCriticalSection Mutex;
	TUniquePtr<FUnrealAICredentialResult> Terminal;
};

class FBrowserCredentialApplicator final : public IUnrealAICredentialApplicator
{
  public:
	explicit FBrowserCredentialApplicator(FUnrealAICredentialDestination InDestination)
		: Destination(MoveTemp(InDestination))
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	int32 ExposedBytes = 0;

  protected:
	bool ApplyCredentialAndDispatch(EUnrealAIAuthScheme, TConstArrayView<uint8> Secret) override
	{
		ExposedBytes = Secret.Num();
		return true;
	}

	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
};

bool ComposeBrowserConnections(
	const FUnrealAIProviderEndpointAuthority &Authority,
	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections,
	FUnrealAICredentialDestination *OutDestination, FString &OutError, const TCHAR *AudienceOverride = nullptr)
{
	OutConnections.Reset();
	FUnrealAIEndpointOrigin Origin;
	if (!FUnrealAIEndpointOrigin::TryParse(TEXT("https://resource.browser.test"), false, Origin, OutError))
	{
		return false;
	}
	const FString Audience(AudienceOverride == nullptr ? TEXT("https://resource.browser.test/v1/responses")
													   : AudienceOverride);
	FUnrealAIEndpointProfileRegistry Endpoints;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> RegisteredEndpoint;
	if (!Endpoints.RegisterProviderSubscriptionEndpoint(Authority, BrowserEndpointName, BrowserModelProviderName,
														Origin, Audience, 1, RegisteredEndpoint, OutError))
	{
		return false;
	}
	FUnrealAICredentialDestination Destination;
	Destination.ModelProviderName = BrowserModelProviderName;
	Destination.AccountAuthProviderName = BrowserProviderName;
	Destination.AuthProfileId = BrowserProfileName;
	Destination.AccountId = BrowserAccountId;
	Destination.TenantRealm = TEXT("tests.browser_oauth.tenant");
	Destination.BillingPrincipalId.Value = FGuid(0x11112222, 0x33334444, 0x55556666, 0x77778888);
	Destination.PayerHandle = TEXT("tests.browser_oauth.subscription");
	Destination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Destination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Destination.EndpointOrigin = Origin;
	Destination.Audience = Audience;
	Destination.ConnectionRevision = 1;
	Destination.EndpointPolicyRevision = 1;
	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionAlias = BrowserConnectionName;
	Connection.EndpointProfileId = BrowserEndpointName;
	Connection.CredentialDestination = Destination;
	FUnrealAIConnectionRegistry Connections(Endpoints.CreateSnapshot());
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> RegisteredConnection;
	if (!Connections.Register(Connection, RegisteredConnection, OutError))
	{
		return false;
	}
	OutConnections = Connections.CreateSnapshot();
	if (OutDestination != nullptr)
	{
		*OutDestination = Destination;
	}
	return true;
}

struct FBrowserCompositionFixture final
{
	FBrowserCompositionFixture()
		: MutableClock(MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 8, 23, 12, 0, 0), 10.0)),
		  Clock(MutableClock), Store(MakeShared<FBrowserTestStore, ESPMode::ThreadSafe>()),
		  Executor(MakeShared<FBrowserTestExecutor, ESPMode::ThreadSafe>(Clock)),
		  RefreshSource(MakeShared<FBrowserTestRefreshSource, ESPMode::ThreadSafe>()), Catalog(Registry)
	{
	}

	bool Create(const bool bStartRecovery = false)
	{
		const FUnrealAIBrowserOAuthCompositionConfig Config = MakeBrowserConfig(bStartRecovery);
		auto Composer =
			[this](const FUnrealAIProviderEndpointAuthority &MintedAuthority,
				   TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections,
				   FString &OutError)
		{ return ComposeBrowserConnections(MintedAuthority, OutConnections, &Destination, OutError); };
		return FUnrealAIBrowserOAuthCompositionRoot::TryCreateAndRegisterWithExecutor(
			Config, Registry, Catalog, Composer, Executor, Store, Clock, RefreshSource, Root, Authority, Error);
	}

	bool StartSignIn(const uint32 Seed, TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> Sink,
					 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle)
	{
		if (!Root.IsValid())
		{
			return false;
		}
		return FUnrealAIBrowserOAuthAccountProviderTestGestureAuthority::StartSignIn(
			*Root->GetAccountProvider(), MakeBrowserSignInRequest(Seed), Sink, Cancellation.GetToken(), OutHandle,
			Error);
	}

	bool StartSignOut(const uint32 Seed, TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> Sink,
					  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle)
	{
		if (!Root.IsValid())
		{
			return false;
		}
		return FUnrealAIBrowserOAuthAccountProviderTestGestureAuthority::StartSignOut(
			*Root->GetAccountProvider(), MakeBrowserSignOutRequest(Seed), Sink, Cancellation.GetToken(), OutHandle,
			Error);
	}

	FString DescribeError() const
	{
		return FString::Printf(TEXT("Provider-access error category=%d code=%d"), static_cast<int32>(Error.Category),
									static_cast<int32>(Error.Code));
	}

	TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedRef<FBrowserTestStore, ESPMode::ThreadSafe> Store;
	TSharedRef<FBrowserTestExecutor, ESPMode::ThreadSafe> Executor;
	TSharedRef<FBrowserTestRefreshSource, ESPMode::ThreadSafe> RefreshSource;
	FUnrealAIAccountAuthProviderRegistry Registry;
	FUnrealAIAccountCatalogRegistry Catalog;
	TSharedPtr<FUnrealAIBrowserOAuthCompositionRoot, ESPMode::ThreadSafe> Root;
	FUnrealAIProviderEndpointAuthority Authority;
	FUnrealAICredentialDestination Destination;
	FUnrealAICancellationSource Cancellation;
	FUnrealAIProviderAccessError Error;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBrowserOAuthVerticalLifecycleTest,
								 "UnrealAI.Auth.BrowserOAuthAccount.SignInPublishesBrokerAndSignOutRevokesThenDeletes",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBrowserOAuthVerticalLifecycleTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FBrowserCompositionFixture Fixture;
	const bool bCreated = Fixture.Create();
	TestTrue(TEXT("Composition registers one complete provider"), bCreated);
	if (!bCreated)
	{
		AddError(Fixture.DescribeError());
		return false;
	}
	TestEqual(TEXT("Provider is registered exactly once"), Fixture.Registry.Num(), 1);
	TestEqual(TEXT("Account is cataloged exactly once"), Fixture.Catalog.Num(), 1);
	TestTrue(TEXT("Registry mints subscription endpoint authority"), Fixture.Authority.IsValid());
	TestEqual(TEXT("Composition freezes one broker connection"), Fixture.Root->GetConnections()->Num(), 1);

	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> SignInSink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignInHandle;
	TestTrue(TEXT("Browser sign-in is admitted"), Fixture.StartSignIn(100, SignInSink, SignInHandle));
	TestTrue(TEXT("Browser sign-in reaches a terminal"), SignInSink->WaitForTerminal());
	TestEqual(TEXT("Browser sign-in succeeds exactly once"), SignInSink->GetTerminalKind(),
				   EUnrealAIAuthEventKind::Succeeded);
	TestEqual(TEXT("Sign-in emits one terminal"), SignInSink->NumTerminals(), 1);
	TestEqual(TEXT("Durable account is Ready"),
				   Fixture.Root->GetAccountProvider()->GetStatus(BrowserProfileName, BrowserAccountId).State,
				   EUnrealAIAccountAuthState::Ready);
	FUnrealAIAccountCatalogView SelectedAccount;
	FUnrealAIProviderAccessError SelectionError;
	TestTrue(TEXT("Exact catalog alias resolves the ready account"),
				  Fixture.Catalog.CreateSnapshot()->ResolveReadyExact(BrowserSelectionName, SelectedAccount,
																	  SelectionError));
	TestEqual(TEXT("Catalog discloses the exact frozen payer"), SelectedAccount.Destination.PayerHandle,
				   FName(TEXT("tests.browser_oauth.subscription")));
	TestEqual(TEXT("One durable envelope is committed"), Fixture.Store->Num(), 1);

	const TSharedRef<FBrowserCredentialSink, ESPMode::ThreadSafe> CredentialSink =
		MakeShared<FBrowserCredentialSink, ESPMode::ThreadSafe>();
	FUnrealAICredentialRequest CredentialRequest;
	CredentialRequest.RequestId.Value = FGuid(0x901, 0x902, 0x903, 0x904);
	CredentialRequest.ConnectionAlias = BrowserConnectionName;
	CredentialRequest.TimeoutSeconds = 5.0f;
	FUnrealAICancellationSource CredentialCancellation;
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> CredentialHandle;
	TestTrue(TEXT("Common broker resolves the committed account"),
				  Fixture.Root->GetCredentialBroker()->StartResolve(CredentialRequest, CredentialSink,
																	CredentialCancellation.GetToken(), CredentialHandle,
																	Fixture.Error));
	FUnrealAICredentialResult CredentialResult;
	TestTrue(TEXT("Common broker publishes a terminal credential context"),
				  CredentialSink->WaitAndTake(CredentialResult));
	TestEqual(TEXT("Credential resolve succeeds"), CredentialResult.Kind, EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("Credential remains opaque before dispatch"), CredentialResult.AccessContext.IsValid());

	TAtomic<bool> bDeleteObservedCompletedRevocation{false};
	Fixture.Store->OnDelete = [&Fixture, &bDeleteObservedCompletedRevocation]()
	{ bDeleteObservedCompletedRevocation.Store(Fixture.Executor->bRevocationCompleted.Load()); };
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> SignOutSink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignOutHandle;
	TestTrue(TEXT("Sign-out is admitted"), Fixture.StartSignOut(200, SignOutSink, SignOutHandle));
	TestTrue(TEXT("Sign-out reaches a terminal"), SignOutSink->WaitForTerminal());
	TestEqual(TEXT("Sign-out succeeds"), SignOutSink->GetTerminalKind(), EUnrealAIAuthEventKind::Succeeded);
	TestEqual(TEXT("Issuer revocation runs exactly once"), Fixture.Executor->RevokeCalls.Load(), 1);
	TestTrue(TEXT("Revocation receives an opaque credential"), Fixture.Executor->bRevocationHadToken.Load());
	TestTrue(TEXT("Revocation uses a fresh cancellation lineage"),
				  Fixture.Executor->bRevocationUsedFreshCancellation.Load());
	TestTrue(TEXT("Revocation reuses exact issuer authority"), Fixture.Executor->bRevocationUsedExactAuthority.Load());
	TestTrue(TEXT("Protected-store delete occurs only after issuer revocation completes"),
				  bDeleteObservedCompletedRevocation.Load());
	TestEqual(TEXT("Sign-out removes the durable record"), Fixture.Store->Num(), 0);
	TestEqual(TEXT("Sign-out publishes SignedOut"),
				   Fixture.Root->GetAccountProvider()->GetStatus(BrowserProfileName, BrowserAccountId).State,
				   EUnrealAIAccountAuthState::SignedOut);

	FBrowserCredentialApplicator Applicator(Fixture.Destination);
	FString DispatchError;
	TestFalse(TEXT("Sign-out invalidates a previously issued lease"),
				   CredentialResult.AccessContext->TryDispatch(Applicator, DispatchError));
	TestEqual(TEXT("Invalidated lease exposes no credential bytes"), Applicator.ExposedBytes, 0);
	Fixture.Root->BeginShutdown();
	TestEqual(TEXT("Composition shutdown withdraws its exact provider registration"), Fixture.Registry.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBrowserOAuthRestartRecoveryTest,
								 "UnrealAI.Auth.BrowserOAuthAccount.RestartRecoversWithoutBrowser",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBrowserOAuthRestartRecoveryTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FBrowserCompositionFixture First;
	const bool bFirstCreated = First.Create();
	TestTrue(TEXT("Initial composition succeeds"), bFirstCreated);
	if (!bFirstCreated)
	{
		AddError(First.DescribeError());
		return false;
	}
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> Sink = MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	TestTrue(TEXT("Initial sign-in starts"), First.StartSignIn(300, Sink, Handle));
	TestTrue(TEXT("Initial sign-in completes"), Sink->WaitForTerminal());
	TestEqual(TEXT("Initial sign-in is successful"), Sink->GetTerminalKind(), EUnrealAIAuthEventKind::Succeeded);
	TestEqual(TEXT("Initial authorization ran once"), First.Executor->AuthorizeCalls.Load(), 1);
	First.Root->BeginShutdown();

	FBrowserCompositionFixture Restarted;
	Restarted.Store = First.Store;
	const bool bRestartedCreated = Restarted.Create(true);
	TestTrue(TEXT("Restarted composition succeeds over the same protected store"), bRestartedCreated);
	if (!bRestartedCreated)
	{
		AddError(Restarted.DescribeError());
		return false;
	}
	TestTrue(TEXT("Startup recovery reaches Ready"),
		WaitForBrowserCondition(
			[&Restarted]()
			{
				return Restarted.Root->GetAccountProvider()->GetStatus(BrowserProfileName, BrowserAccountId).State ==
					   EUnrealAIAccountAuthState::Ready;
			}));
	TestEqual(TEXT("Recovery never opens browser authorization"), Restarted.Executor->AuthorizeCalls.Load(), 0);
	TestEqual(TEXT("Recovery retains the exact committed envelope"), Restarted.Store->Num(), 1);
	Restarted.Root->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBrowserOAuthRecoverySignOutRaceTest,
								 "UnrealAI.Auth.BrowserOAuthAccount.SignOutCancelsAndFencesStartupRecovery",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBrowserOAuthRecoverySignOutRaceTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FBrowserCompositionFixture First;
	const bool bFirstCreated = First.Create();
	TestTrue(TEXT("Recovery-race seed composition succeeds"), bFirstCreated);
	if (!bFirstCreated)
	{
		AddError(First.DescribeError());
		return false;
	}
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> SignInSink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignInHandle;
	TestTrue(TEXT("Recovery-race seed sign-in starts"), First.StartSignIn(350, SignInSink, SignInHandle));
	TestTrue(TEXT("Recovery-race seed sign-in completes"), SignInSink->WaitForTerminal());
	TestEqual(TEXT("Recovery-race seed sign-in succeeds"), SignInSink->GetTerminalKind(),
				   EUnrealAIAuthEventKind::Succeeded);
	First.Root->BeginShutdown();

	First.Store->BlockNextLoad();
	FBrowserCompositionFixture Restarted;
	Restarted.Store = First.Store;
	const bool bRestartedCreated = Restarted.Create(true);
	TestTrue(TEXT("Recovery-race restart composition succeeds"), bRestartedCreated);
	if (!bRestartedCreated)
	{
		First.Store->ReleaseBlockedLoad();
		AddError(Restarted.DescribeError());
		return false;
	}
	TestTrue(TEXT("Startup recovery enters the protected store"), Restarted.Store->WaitForBlockedLoad());
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> SignOutSink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignOutHandle;
	TestTrue(TEXT("Sign-out is admitted while startup recovery is physical"),
				  Restarted.StartSignOut(360, SignOutSink, SignOutHandle));
	Restarted.Store->ReleaseBlockedLoad();
	TestTrue(TEXT("Sign-out waits for recovery settlement and completes"), SignOutSink->WaitForTerminal());
	TestEqual(TEXT("Sign-out wins the startup-recovery race"), SignOutSink->GetTerminalKind(),
				   EUnrealAIAuthEventKind::Succeeded);
	TestEqual(TEXT("Recovery race never opens browser authorization"), Restarted.Executor->AuthorizeCalls.Load(), 0);
	TestEqual(TEXT("Recovery race revokes the stored issuer grant"), Restarted.Executor->RevokeCalls.Load(), 1);
	TestEqual(TEXT("Recovery race removes the exact durable record"), Restarted.Store->Num(), 0);
	TestEqual(TEXT("Recovery cannot revive the signed-out account"),
				   Restarted.Root->GetAccountProvider()->GetStatus(BrowserProfileName, BrowserAccountId).State,
				   EUnrealAIAccountAuthState::SignedOut);
	Restarted.Root->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBrowserOAuthCancellationTest,
								 "UnrealAI.Auth.BrowserOAuthAccount.CancelledAuthorizationCannotCommitOrRevive",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBrowserOAuthCancellationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FBrowserCompositionFixture Fixture;
	Fixture.Executor->BlockAuthorization(true);
	const bool bCreated = Fixture.Create();
	TestTrue(TEXT("Cancellation fixture composes"), bCreated);
	if (!bCreated)
	{
		AddError(Fixture.DescribeError());
		return false;
	}
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> Sink = MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	TestTrue(TEXT("Blocked browser sign-in starts"), Fixture.StartSignIn(400, Sink, Handle));
	TestTrue(TEXT("Authorization executor entered"), Fixture.Executor->WaitForAuthorization());
	Handle->Cancel();
	TestTrue(TEXT("Logical cancellation publishes promptly"), Sink->WaitForTerminal());
	TestEqual(TEXT("Cancellation owns the terminal"), Sink->GetTerminalKind(), EUnrealAIAuthEventKind::Cancelled);
	Fixture.Executor->ReleaseAuthorization();
	TestTrue(TEXT("Cancelled worker physically settles"),
				  WaitForBrowserCondition(
					  [&Fixture]() { return Fixture.Root->GetAccountProvider()->GetPhysicalOperationCount() == 0; }));
	TestEqual(TEXT("Late issued grant is compensatingly revoked"), Fixture.Executor->RevokeCalls.Load(), 1);
	TestEqual(TEXT("Cancelled authorization leaves no durable credential"), Fixture.Store->Num(), 0);
	TestEqual(TEXT("Cancelled authorization cannot revive Ready"),
				   Fixture.Root->GetAccountProvider()->GetStatus(BrowserProfileName, BrowserAccountId).State,
				   EUnrealAIAccountAuthState::SignedOut);
	TestEqual(TEXT("Cancellation publishes one terminal"), Sink->NumTerminals(), 1);
	Fixture.Root->BeginShutdown();
	TestEqual(TEXT("Successful retry unregisters during shutdown"), Fixture.Registry.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBrowserOAuthConcurrentSignOutTest,
								 "UnrealAI.Auth.BrowserOAuthAccount.SignOutFencesConcurrentSignIn",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBrowserOAuthConcurrentSignOutTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FBrowserCompositionFixture Fixture;
	Fixture.Executor->BlockAuthorization(true);
	const bool bCreated = Fixture.Create();
	TestTrue(TEXT("Race fixture composes"), bCreated);
	if (!bCreated)
	{
		AddError(Fixture.DescribeError());
		return false;
	}
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> SignInSink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignInHandle;
	TestTrue(TEXT("Blocked sign-in starts"), Fixture.StartSignIn(500, SignInSink, SignInHandle));
	TestTrue(TEXT("Blocked sign-in reaches issuer executor"), Fixture.Executor->WaitForAuthorization());
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> SignOutSink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignOutHandle;
	TestTrue(TEXT("Sign-out supersedes an in-flight sign-in"), Fixture.StartSignOut(510, SignOutSink, SignOutHandle));
	TestTrue(TEXT("Superseded sign-in is cancelled"), SignInSink->WaitForTerminal());
	TestEqual(TEXT("Sign-in receives cancellation terminal"), SignInSink->GetTerminalKind(),
				   EUnrealAIAuthEventKind::Cancelled);
	Fixture.Executor->ReleaseAuthorization();
	TestTrue(TEXT("Sign-out waits for settlement and completes"), SignOutSink->WaitForTerminal());
	TestEqual(TEXT("Sign-out wins the race"), SignOutSink->GetTerminalKind(), EUnrealAIAuthEventKind::Succeeded);
	TestEqual(TEXT("Issued race credential is revoked once"), Fixture.Executor->RevokeCalls.Load(), 1);
	TestEqual(TEXT("Sign-out race leaves no durable record"), Fixture.Store->Num(), 0);
	TestEqual(TEXT("Late sign-in cannot revive account"),
				   Fixture.Root->GetAccountProvider()->GetStatus(BrowserProfileName, BrowserAccountId).State,
				   EUnrealAIAccountAuthState::SignedOut);
	Fixture.Root->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBrowserOAuthPersistenceFailureTest,
								 "UnrealAI.Auth.BrowserOAuthAccount.PersistenceFailureRevokesAndFailsClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBrowserOAuthPersistenceFailureTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FBrowserCompositionFixture Fixture;
	Fixture.Store->bFailNextStore.Store(true);
	const bool bCreated = Fixture.Create();
	TestTrue(TEXT("Persistence fixture composes"), bCreated);
	if (!bCreated)
	{
		AddError(Fixture.DescribeError());
		return false;
	}
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> Sink = MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> Handle;
	TestTrue(TEXT("Persistence-fault sign-in starts"), Fixture.StartSignIn(600, Sink, Handle));
	TestTrue(TEXT("Persistence-fault sign-in completes"), Sink->WaitForTerminal());
	TestEqual(TEXT("Persistence failure publishes Failed"), Sink->GetTerminalKind(), EUnrealAIAuthEventKind::Failed);
	TestEqual(TEXT("Issued grant is revoked after persistence failure"), Fixture.Executor->RevokeCalls.Load(), 1);
	TestTrue(TEXT("Compensation uses a fresh lineage"), Fixture.Executor->bRevocationUsedFreshCancellation.Load());
	TestEqual(TEXT("Persistence failure leaves no local record"), Fixture.Store->Num(), 0);
	TestEqual(TEXT("Persistence failure cannot publish Ready"),
				   Fixture.Root->GetAccountProvider()->GetStatus(BrowserProfileName, BrowserAccountId).State,
				   EUnrealAIAccountAuthState::Failed);
	Fixture.Root->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBrowserOAuthRevocationFailureTest,
								 "UnrealAI.Auth.BrowserOAuthAccount.RevocationFailureRetainsRecordAndQuarantine",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBrowserOAuthRevocationFailureTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FBrowserCompositionFixture Fixture;
	const bool bCreated = Fixture.Create();
	TestTrue(TEXT("Revocation-failure fixture composes"), bCreated);
	if (!bCreated)
	{
		AddError(Fixture.DescribeError());
		return false;
	}
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> SignInSink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> SignInHandle;
	TestTrue(TEXT("Revocation-failure seed sign-in starts"), Fixture.StartSignIn(650, SignInSink, SignInHandle));
	TestTrue(TEXT("Revocation-failure seed sign-in completes"), SignInSink->WaitForTerminal());
	TestEqual(TEXT("Revocation-failure seed sign-in succeeds"), SignInSink->GetTerminalKind(),
				   EUnrealAIAuthEventKind::Succeeded);

	const TSharedRef<FBrowserCredentialSink, ESPMode::ThreadSafe> CredentialSink =
		MakeShared<FBrowserCredentialSink, ESPMode::ThreadSafe>();
	FUnrealAICredentialRequest CredentialRequest;
	CredentialRequest.RequestId.Value = FGuid(0xa01, 0xa02, 0xa03, 0xa04);
	CredentialRequest.ConnectionAlias = BrowserConnectionName;
	CredentialRequest.TimeoutSeconds = 5.0f;
	FUnrealAICancellationSource CredentialCancellation;
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> CredentialHandle;
	TestTrue(TEXT("Revocation-failure fixture acquires an opaque lease"),
				  Fixture.Root->GetCredentialBroker()->StartResolve(CredentialRequest, CredentialSink,
																	CredentialCancellation.GetToken(), CredentialHandle,
																	Fixture.Error));
	FUnrealAICredentialResult CredentialResult;
	TestTrue(TEXT("Revocation-failure lease resolve completes"), CredentialSink->WaitAndTake(CredentialResult));
	TestEqual(TEXT("Revocation-failure lease resolve succeeds"), CredentialResult.Kind,
				   EUnrealAICredentialResultKind::Succeeded);

	Fixture.Executor->bFailRevocation.Store(true);
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> FailedSignOutSink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> FailedSignOutHandle;
	TestTrue(TEXT("Revocation-failure sign-out starts"),
				  Fixture.StartSignOut(660, FailedSignOutSink, FailedSignOutHandle));
	TestTrue(TEXT("Revocation-failure sign-out completes"), FailedSignOutSink->WaitForTerminal());
	TestEqual(TEXT("Remote revocation failure fails sign-out"), FailedSignOutSink->GetTerminalKind(),
				   EUnrealAIAuthEventKind::Failed);
	TestEqual(TEXT("Failed revocation is attempted exactly once"), Fixture.Executor->RevokeCalls.Load(), 1);
	TestEqual(TEXT("Failed revocation never deletes the protected record"), Fixture.Store->DeleteCalls.Load(), 0);
	TestEqual(TEXT("Failed revocation retains the exact protected record"), Fixture.Store->Num(), 1);
	TestEqual(TEXT("Failed revocation leaves the account quarantined"),
				   Fixture.Root->GetAccountProvider()->GetStatus(BrowserProfileName, BrowserAccountId).State,
				   EUnrealAIAccountAuthState::ReauthenticationRequired);
	FBrowserCredentialApplicator Applicator(Fixture.Destination);
	FString DispatchError;
	TestFalse(TEXT("The sign-out fence invalidates a lease even when remote revocation fails"),
				   CredentialResult.AccessContext->TryDispatch(Applicator, DispatchError));
	TestEqual(TEXT("A quarantined lease exposes no credential bytes"), Applicator.ExposedBytes, 0);
	TestTrue(TEXT("Failed sign-out physically settles before retry"),
				  WaitForBrowserCondition(
					  [&Fixture]() { return Fixture.Root->GetAccountProvider()->GetPhysicalOperationCount() == 0; }));

	Fixture.Executor->bFailRevocation.Store(false);
	const TSharedRef<FBrowserAuthSink, ESPMode::ThreadSafe> RetrySink =
		MakeShared<FBrowserAuthSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> RetryHandle;
	TestTrue(TEXT("Sign-out can retry the retained record"), Fixture.StartSignOut(670, RetrySink, RetryHandle));
	TestTrue(TEXT("Retried sign-out completes"), RetrySink->WaitForTerminal());
	TestEqual(TEXT("Retried sign-out succeeds"), RetrySink->GetTerminalKind(), EUnrealAIAuthEventKind::Succeeded);
	TestEqual(TEXT("Retry revokes the retained credential"), Fixture.Executor->RevokeCalls.Load(), 2);
	TestEqual(TEXT("Retry deletes only after successful revocation"), Fixture.Store->DeleteCalls.Load(), 1);
	TestEqual(TEXT("Retry removes the durable record"), Fixture.Store->Num(), 0);
	Fixture.Root->BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBrowserOAuthCompositionRollbackTest,
								 "UnrealAI.Auth.BrowserOAuthAccount.CompositionFailureRollsBackRegistration",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBrowserOAuthCompositionRollbackTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FBrowserCompositionFixture Fixture;
	const FUnrealAIBrowserOAuthCompositionConfig Config = MakeBrowserConfig(false);
	auto EmptyComposer = [](const FUnrealAIProviderEndpointAuthority &,
							TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections,
							FString &)
	{
		FUnrealAIEndpointProfileRegistry Endpoints;
		FUnrealAIConnectionRegistry Connections(Endpoints.CreateSnapshot());
		OutConnections = Connections.CreateSnapshot();
		return true;
	};
	TestFalse(TEXT("A composition without an exact account-bound connection fails closed"),
				   FUnrealAIBrowserOAuthCompositionRoot::TryCreateAndRegisterWithExecutor(
					   Config, Fixture.Registry, Fixture.Catalog, EmptyComposer, Fixture.Executor, Fixture.Store,
					   Fixture.Clock, Fixture.RefreshSource, Fixture.Root, Fixture.Authority, Fixture.Error));
	TestEqual(TEXT("Unbound connection composition rolls back registration"), Fixture.Registry.Num(), 0);
	TestEqual(TEXT("Unbound connection composition publishes no catalog entry"), Fixture.Catalog.Num(), 0);
	TestFalse(TEXT("Unbound connection composition returns no root"), Fixture.Root.IsValid());
	TestFalse(TEXT("Unbound connection composition returns no authority"), Fixture.Authority.IsValid());
	auto WrongAudienceComposer =
		[](const FUnrealAIProviderEndpointAuthority &Authority,
		   TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections,
		   FString &OutError)
	{
		return ComposeBrowserConnections(Authority, OutConnections, nullptr, OutError,
										 TEXT("https://resource.browser.test/v1/wrong"));
	};
	TestFalse(TEXT("A connection with issuer-audience drift fails closed"),
				   FUnrealAIBrowserOAuthCompositionRoot::TryCreateAndRegisterWithExecutor(
					   Config, Fixture.Registry, Fixture.Catalog, WrongAudienceComposer, Fixture.Executor,
					   Fixture.Store, Fixture.Clock, Fixture.RefreshSource, Fixture.Root, Fixture.Authority,
					   Fixture.Error));
	TestEqual(TEXT("Audience-drift composition rolls back registration"), Fixture.Registry.Num(), 0);
	TestFalse(TEXT("Audience-drift composition returns no root"), Fixture.Root.IsValid());
	TestFalse(TEXT("Audience-drift composition returns no authority"), Fixture.Authority.IsValid());

	auto FailingComposer =
		[](const FUnrealAIProviderEndpointAuthority &,
		   TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> &OutConnections,
		   FString &OutError)
	{
		OutConnections.Reset();
		OutError = TEXT("fixture connection failure");
		return false;
	};
	TestFalse(TEXT("Failed connection composition fails the root"),
				   FUnrealAIBrowserOAuthCompositionRoot::TryCreateAndRegisterWithExecutor(
					   Config, Fixture.Registry, Fixture.Catalog, FailingComposer, Fixture.Executor, Fixture.Store,
					   Fixture.Clock, Fixture.RefreshSource, Fixture.Root, Fixture.Authority, Fixture.Error));
	TestEqual(TEXT("Failed composition removes its exact provider registration"), Fixture.Registry.Num(), 0);
	TestFalse(TEXT("Failed composition returns no root"), Fixture.Root.IsValid());
	TestFalse(TEXT("Failed composition returns no authority"), Fixture.Authority.IsValid());
	const bool bCreated = Fixture.Create();
	TestTrue(TEXT("A clean retry can register the same provider identity"), bCreated);
	if (!bCreated)
	{
		AddError(Fixture.DescribeError());
		return false;
	}
	TestEqual(TEXT("Successful retry owns one registration"), Fixture.Registry.Num(), 1);
	Fixture.Root->BeginShutdown();
	return true;
}

#endif // WITH_AUTOMATION_TESTS
