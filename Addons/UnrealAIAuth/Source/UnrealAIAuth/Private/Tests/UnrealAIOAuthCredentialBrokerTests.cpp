// Copyright EngineWorks. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"

#include "Auth/UnrealAIAccountAuthProviderRegistry.h"
#include "Auth/UnrealAIConnectionRegistry.h"
#include "Auth/UnrealAIMemorySecretStore.h"
#include "Auth/UnrealAIOAuthCredentialBroker.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

namespace
{
constexpr TCHAR OAuthStoreName[] = TEXT("tests.oauth.store");
constexpr TCHAR OAuthAuthProviderName[] = TEXT("tests.oauth.auth");
constexpr TCHAR OAuthModelProviderName[] = TEXT("tests.oauth.model");
constexpr TCHAR OAuthProfileName[] = TEXT("tests.oauth.profile");
constexpr TCHAR OAuthEndpointName[] = TEXT("tests.oauth.endpoint");

FUnrealAIProviderAccessError MakeOAuthTestError(const EUnrealAIErrorCategory Category,
												const EUnrealAIProviderAccessErrorCode Code,
												const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

bool MakeOAuthTestSecret(const ANSICHAR *Text, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	TArray<uint8> Bytes;
	const int32 Length = FCStringAnsi::Strlen(Text);
	Bytes.Append(reinterpret_cast<const uint8 *>(Text), Length);
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, OutError);
}

FUnrealAIAccessAccountId MakeOAuthTestAccount(const uint32 Seed)
{
	FUnrealAIAccessAccountId Account;
	Account.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	return Account;
}

FUnrealAISecretHandle MakeOAuthTestHandle(const uint32 Seed)
{
	FUnrealAISecretHandle Handle;
	Handle.StoreName = OAuthStoreName;
	Handle.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	return Handle;
}

class FOAuthTestAccountAuthProvider final : public IUnrealAIAccountAuthProvider
{
  public:
	FName GetProviderName() const override
	{
		return OAuthAuthProviderName;
	}

	FUnrealAIAccountAuthCapabilities DescribeCapabilities() const override
	{
		FUnrealAIAccountAuthCapabilities Capabilities;
		Capabilities.bDeviceCode = true;
		Capabilities.bRefresh = true;
		Capabilities.bRevocation = true;
		return Capabilities;
	}

	FUnrealAIProviderAccessDescriptor DescribeAccess() const override
	{
		FUnrealAIProviderAccessDescriptor Access;
		Access.ModelProviderName = OAuthModelProviderName;
		Access.AccountAuthProviderName = OAuthAuthProviderName;
		Access.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
		Access.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
		Access.Availability = EUnrealAIProviderAccessAvailability::Available;
		Access.SupportClassification = EUnrealAIProviderAccessSupportClassification::Supported;
		return Access;
	}

	FUnrealAIAccountStatus GetStatus(const FName AuthProfileId, const FUnrealAIAccessAccountId &) const override
	{
		FUnrealAIAccountStatus Status;
		Status.ProviderName = OAuthAuthProviderName;
		Status.AuthProfileId = AuthProfileId;
		Status.State = EUnrealAIAccountAuthState::SignedOut;
		return Status;
	}

	bool StartSignIn(const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIInteractiveAuthRequest &,
					 TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe>, const FUnrealAICancellationToken &,
					 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					 FUnrealAIProviderAccessError &OutError) override
	{
		OutHandle.Reset();
		OutError = MakeOAuthTestError(EUnrealAIErrorCategory::UnsupportedCapability,
									  EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
		return false;
	}

	bool StartSignOut(const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIAccountAuthRequest &,
					  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe>, const FUnrealAICancellationToken &,
					  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError) override
	{
		OutHandle.Reset();
		OutError = MakeOAuthTestError(EUnrealAIErrorCategory::UnsupportedCapability,
									  EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
		return false;
	}
};

FUnrealAIConnectionDescriptor MakeOAuthTestConnection(const FName Alias, const FUnrealAIAccessAccountId &Account,
													  FString &OutError, const FName AuthProfileId = OAuthProfileName)
{
	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionRevision = 7;
	Connection.ConnectionAlias = Alias;
	Connection.EndpointProfileId = OAuthEndpointName;
	FUnrealAICredentialDestination &Destination = Connection.CredentialDestination;
	Destination.ModelProviderName = OAuthModelProviderName;
	Destination.AccountAuthProviderName = OAuthAuthProviderName;
	Destination.AuthProfileId = AuthProfileId;
	Destination.AccountId = Account;
	Destination.TenantRealm = TEXT("tests.oauth.tenant");
	Destination.BillingPrincipalId.Value = FGuid(101, 102, 103, 104);
	Destination.PayerHandle = TEXT("tests.oauth.subscription");
	Destination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Destination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Destination.Audience = TEXT("oauth-subscription-inference");
	Destination.ConnectionRevision = Connection.ConnectionRevision;
	Destination.EndpointPolicyRevision = 11;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://oauth.example.com"), false, Destination.EndpointOrigin, OutError);
	return Connection;
}

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe>
MakeOAuthTestConnections(const TArray<FUnrealAIConnectionDescriptor> &Connections, FString &OutError)
{
	FUnrealAIAccountAuthProviderRegistry AuthProviders;
	const TSharedRef<FOAuthTestAccountAuthProvider, ESPMode::ThreadSafe> AuthProvider =
		MakeShared<FOAuthTestAccountAuthProvider, ESPMode::ThreadSafe>();
	FUnrealAIProviderEndpointAuthority Authority;
	AuthProviders.Register(AuthProvider, Authority, OutError);

	FUnrealAIEndpointProfileRegistry EndpointProfiles;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> RegisteredEndpoint;
	if (!Connections.IsEmpty())
	{
		const FUnrealAICredentialDestination &Destination = Connections[0].CredentialDestination;
		EndpointProfiles.RegisterProviderSubscriptionEndpoint(
			Authority, OAuthEndpointName, Destination.ModelProviderName, Destination.EndpointOrigin,
			Destination.Audience, Destination.EndpointPolicyRevision, RegisteredEndpoint, OutError);
	}
	const TSharedRef<FUnrealAIConnectionRegistry, ESPMode::ThreadSafe> Registry =
		MakeShared<FUnrealAIConnectionRegistry, ESPMode::ThreadSafe>(EndpointProfiles.CreateSnapshot());
	for (const FUnrealAIConnectionDescriptor &Connection : Connections)
	{
		TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Registered;
		Registry->Register(Connection, Registered, OutError);
	}
	return Registry->CreateSnapshot();
}

FUnrealAICredentialRequest MakeOAuthTestRequest(const uint32 Seed, const FName ConnectionAlias,
												const float TimeoutSeconds = 30.0f)
{
	FUnrealAICredentialRequest Request;
	Request.RequestId.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	Request.ConnectionAlias = ConnectionAlias;
	Request.TimeoutSeconds = TimeoutSeconds;
	return Request;
}

FUnrealAIOAuthCredentialBinding MakeOAuthTestBinding(const FUnrealAIAccessAccountId &Account,
													 const FUnrealAISecretHandle &Handle,
													 const bool bRequiresProtectedSecondary,
													 const FName AuthProfileId = OAuthProfileName)
{
	FUnrealAIOAuthCredentialBinding Binding;
	Binding.ProviderName = OAuthAuthProviderName;
	Binding.AuthProfileId = AuthProfileId;
	Binding.AccountId = Account;
	Binding.SecretHandle = Handle;
	Binding.bRequiresProtectedSecondary = bRequiresProtectedSecondary;
	return Binding;
}

bool StoreOAuthTestEnvelope(FUnrealAIMemorySecretStore &Store,
							TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
							const FUnrealAIOAuthCredentialBinding &Binding, const FDateTime &ExpiryUtc,
							const bool bIncludeRefresh, const bool bIncludeRouting, FString &OutError)
{
	FUnrealAIOAuthTokenSet Tokens;
	if (!MakeOAuthTestSecret("access-original", Tokens.AccessToken, OutError) ||
		(bIncludeRefresh && !MakeOAuthTestSecret("refresh-original", Tokens.RefreshToken, OutError)) ||
		(bIncludeRouting && !MakeOAuthTestSecret("route-original", Tokens.AccountRoutingValue, OutError)))
	{
		return false;
	}
	Tokens.AccessTokenExpiresAtUtc = ExpiryUtc;
	FUnrealAIOAuthTokenEnvelope Envelope;
	const FUnrealAIOAuthTokenEnvelopeBinding EnvelopeBinding{Binding.ProviderName, Binding.AuthProfileId,
															 Binding.AccountId};
	if (!FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(EnvelopeBinding, MoveTemp(Tokens), Clock->UtcNow(), Envelope,
													 OutError))
	{
		return false;
	}
	FUnrealAISecretValue Encoded;
	if (!FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(Envelope), Clock->UtcNow(), Encoded, OutError))
	{
		return false;
	}
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context, OutError))
	{
		return false;
	}
	uint64 Revision = 0;
	FUnrealAIProviderAccessError StoreError;
	return Store.Store(Context, Binding.SecretHandle, Encoded, 0, Revision, StoreError) ==
			   EUnrealAISecretStoreResult::Succeeded &&
		   Revision == 1;
}

bool StoreOAuthTestRaw(FUnrealAIMemorySecretStore &Store, TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
					   const FUnrealAISecretHandle &Handle, const ANSICHAR *Raw, FString &OutError)
{
	FUnrealAISecretValue Value;
	if (!MakeOAuthTestSecret(Raw, Value, OutError))
	{
		return false;
	}
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context, OutError))
	{
		return false;
	}
	uint64 Revision = 0;
	FUnrealAIProviderAccessError StoreError;
	return Store.Store(Context, Handle, Value, 0, Revision, StoreError) == EUnrealAISecretStoreResult::Succeeded &&
		   Revision == 1;
}

bool LoadOAuthTestRevision(FUnrealAIMemorySecretStore &Store,
						   TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
						   const FUnrealAISecretHandle &Handle, uint64 &OutRevision)
{
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	FString Error;
	if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context, Error))
	{
		return false;
	}
	FUnrealAISecretValue Encoded;
	FUnrealAIProviderAccessError StoreError;
	return Store.Load(Context, Handle, Encoded, OutRevision, StoreError) == EUnrealAISecretStoreResult::Succeeded;
}

bool WaitForOAuthTestCondition(TFunctionRef<bool()> Predicate, const double TimeoutSeconds = 3.0)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!Predicate() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return Predicate();
}

class FOAuthAdmissionGateClock final : public IUnrealAIClock
{
  public:
	FOAuthAdmissionGateClock()
		: Entered(FPlatformProcess::GetSynchEventFromPool(true)),
		  ReleaseEvent(FPlatformProcess::GetSynchEventFromPool(true))
	{
		ReleaseEvent->Trigger();
	}

	~FOAuthAdmissionGateClock() override
	{
		ReleaseEvent->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseEvent);
	}

	FDateTime UtcNow() const override
	{
		return FDateTime(2026, 7, 23, 13, 45, 0);
	}

	double MonotonicSeconds() const override
	{
		if (bBlockNextMonotonic.Exchange(false))
		{
			Entered->Trigger();
			ReleaseEvent->Wait(5000);
		}
		return 27.0;
	}

	double WorldSeconds() const
	{
		return 27.0;
	}

	void BlockNextMonotonicSample()
	{
		Entered->Reset();
		ReleaseEvent->Reset();
		bBlockNextMonotonic.Store(true);
	}

	bool WaitUntilBlocked(const uint32 TimeoutMilliseconds = 3000) const
	{
		return Entered->Wait(TimeoutMilliseconds);
	}

	void Release()
	{
		ReleaseEvent->Trigger();
	}

  private:
	mutable TAtomic<bool> bBlockNextMonotonic{false};
	FEvent *Entered = nullptr;
	FEvent *ReleaseEvent = nullptr;
};

class FOAuthTestCredentialSink final : public IUnrealAICredentialResultSink
{
  public:
	explicit FOAuthTestCredentialSink(TFunction<void()> InDeliveryObserver = {})
		: Event(FPlatformProcess::GetSynchEventFromPool(true)), DeliveryObserver(MoveTemp(InDeliveryObserver))
	{
	}

	~FOAuthTestCredentialSink() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	void EnqueueCredentialResult(FUnrealAICredentialResult &&Result) override
	{
		if (DeliveryObserver)
		{
			DeliveryObserver();
		}
		{
			FScopeLock Lock(&Mutex);
			++Count;
			LastResult = MakeUnique<FUnrealAICredentialResult>(MoveTemp(Result));
		}
		Event->Trigger();
	}

	bool WaitAndTake(FUnrealAICredentialResult &OutResult, const uint32 TimeoutMilliseconds = 3000)
	{
		if (!Event->Wait(TimeoutMilliseconds))
		{
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (!LastResult.IsValid())
		{
			return false;
		}
		OutResult = MoveTemp(*LastResult);
		LastResult.Reset();
		return true;
	}

	int32 GetCount() const
	{
		FScopeLock Lock(&Mutex);
		return Count;
	}

  private:
	mutable FCriticalSection Mutex;
	FEvent *Event = nullptr;
	TUniquePtr<FUnrealAICredentialResult> LastResult;
	TFunction<void()> DeliveryObserver;
	int32 Count = 0;
};

class FOAuthTestCredentialApplicator final : public IUnrealAICredentialApplicator
{
  public:
	explicit FOAuthTestCredentialApplicator(FUnrealAICredentialDestination InDestination)
		: Destination(MoveTemp(InDestination))
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	bool bPairApplied = false;
	int32 PrimaryBytes = 0;
	int32 SecondaryBytes = 0;

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme Scheme, const TConstArrayView<uint8> Secret) override
	{
		PrimaryBytes = Secret.Num();
		return Scheme == EUnrealAIAuthScheme::OAuthBearer && !Secret.IsEmpty();
	}

	bool ApplyCredentialAndProtectedSecondaryAndDispatch(const EUnrealAIAuthScheme Scheme,
														 const TConstArrayView<uint8> Secret,
														 const TConstArrayView<uint8> ProtectedSecondary) override
	{
		PrimaryBytes = Secret.Num();
		SecondaryBytes = ProtectedSecondary.Num();
		bPairApplied = Scheme == EUnrealAIAuthScheme::OAuthBearer && !Secret.IsEmpty() && !ProtectedSecondary.IsEmpty();
		return bPairApplied;
	}

	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
};

class FOAuthTestRefreshSource final : public IUnrealAIOAuthCredentialRefreshSource
{
  public:
	explicit FOAuthTestRefreshSource(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
									 const bool bInBlock = false, const bool bInIgnoreCancellation = false)
		: Clock(MoveTemp(InClock)), bBlock(bInBlock), bIgnoreCancellation(bInIgnoreCancellation),
		  Entered(FPlatformProcess::GetSynchEventFromPool(true)),
		  ReleaseEvent(FPlatformProcess::GetSynchEventFromPool(true))
	{
		if (!bBlock)
		{
			ReleaseEvent->Trigger();
		}
	}

	~FOAuthTestRefreshSource() override
	{
		ReleaseEvent->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseEvent);
	}

	FName GetProviderName() const override
	{
		return OAuthAuthProviderName;
	}

	bool Refresh(FUnrealAIOAuthTokenEnvelope &InOutEnvelope, const double,
				 const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) override
	{
		++Calls;
		Entered->Trigger();
		ReleaseEvent->Wait(5000);
		if ((!bIgnoreCancellation && Cancellation.IsCancellationRequested()) || !bSucceed.Load())
		{
			OutError = Cancellation.IsCancellationRequested()
						   ? MakeOAuthTestError(EUnrealAIErrorCategory::Cancelled,
												EUnrealAIProviderAccessErrorCode::CredentialCancelled)
						   : (ForcedFailure.IsError()
								  ? ForcedFailure
								  : MakeOAuthTestError(EUnrealAIErrorCategory::Provider,
													   EUnrealAIProviderAccessErrorCode::CredentialFailed, true));
			return false;
		}

		FUnrealAIOAuthTokenSet Refreshed;
		FString Error;
		if (!MakeOAuthTestSecret("access-refreshed", Refreshed.AccessToken, Error) ||
			(bRotateRefresh.Load() && !MakeOAuthTestSecret("refresh-rotated", Refreshed.RefreshToken, Error)))
		{
			OutError = MakeOAuthTestError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
			return false;
		}
		Refreshed.AccessTokenExpiresAtUtc = Clock->UtcNow() + FTimespan::FromHours(1);
		if (!InOutEnvelope.TryApplyRefresh(MoveTemp(Refreshed), Clock->UtcNow(), Error))
		{
			OutError = MakeOAuthTestError(EUnrealAIErrorCategory::Provider,
										  EUnrealAIProviderAccessErrorCode::CredentialFailed);
			return false;
		}
		OutError = {};
		return true;
	}

	void Release()
	{
		ReleaseEvent->Trigger();
	}

	bool WaitUntilEntered(const uint32 TimeoutMilliseconds = 3000) const
	{
		return Entered->Wait(TimeoutMilliseconds);
	}

	TAtomic<int32> Calls{0};
	TAtomic<bool> bSucceed{true};
	TAtomic<bool> bRotateRefresh{true};
	FUnrealAIProviderAccessError ForcedFailure;

  private:
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	bool bBlock = false;
	bool bIgnoreCancellation = false;
	FEvent *Entered = nullptr;
	FEvent *ReleaseEvent = nullptr;
};

class FOAuthTestStore final : public IUnrealAISecretStore
{
  public:
	FOAuthTestStore(TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> InInner,
					const int32 InLoadBarrierTarget = 0)
		: Inner(MoveTemp(InInner)), LoadBarrierTarget(InLoadBarrierTarget),
		  LoadBarrier(FPlatformProcess::GetSynchEventFromPool(true))
	{
		if (LoadBarrierTarget == 0)
		{
			LoadBarrier->Trigger();
		}
	}

	~FOAuthTestStore() override
	{
		LoadBarrier->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(LoadBarrier);
	}

	FName GetStoreName() const override
	{
		return Inner->GetStoreName();
	}

	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override
	{
		return Inner->DescribeCapabilities();
	}

	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &Context,
									const FUnrealAISecretHandle &Handle, FUnrealAISecretValue &OutValue,
									uint64 &OutRevision, FUnrealAIProviderAccessError &OutError) override
	{
		const int32 Forced = ForcedLoadResult.Exchange(-1);
		if (Forced >= 0)
		{
			OutValue.Reset();
			OutRevision = 0;
			const EUnrealAISecretStoreResult Result = static_cast<EUnrealAISecretStoreResult>(Forced);
			if (Result == EUnrealAISecretStoreResult::Locked)
			{
				OutError = MakeOAuthTestError(EUnrealAIErrorCategory::Busy,
											  EUnrealAIProviderAccessErrorCode::SecretStoreLocked, true);
			}
			else if (Result == EUnrealAISecretStoreResult::Unavailable)
			{
				OutError = MakeOAuthTestError(EUnrealAIErrorCategory::Persistence,
											  EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, true);
			}
			else if (Result == EUnrealAISecretStoreResult::Corrupt)
			{
				OutError = MakeOAuthTestError(EUnrealAIErrorCategory::Persistence,
											  EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
			}
			return Result;
		}
		const EUnrealAISecretStoreResult Result = Inner->Load(Context, Handle, OutValue, OutRevision, OutError);
		const int32 Count = ++LoadCalls;
		if (LoadBarrierTarget > 0 && Count <= LoadBarrierTarget)
		{
			if (Count == LoadBarrierTarget)
			{
				LoadBarrier->Trigger();
			}
			LoadBarrier->Wait(5000);
			++ReturnedLoads;
		}
		return Result;
	}

	void ForceNextLoad(const EUnrealAISecretStoreResult Result)
	{
		ForcedLoadResult.Store(static_cast<int32>(Result));
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &Context,
									 const FUnrealAISecretHandle &Handle, const FUnrealAISecretValue &Value,
									 const uint64 ExpectedRevision, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		++StoreCalls;
		if (bCommitWinnerThenConflict.Load())
		{
			uint64 WinningRevision = 0;
			FUnrealAIProviderAccessError WinningError;
			const EUnrealAISecretStoreResult WinningResult =
				Inner->Store(Context, Handle, Value, ExpectedRevision, WinningRevision, WinningError);
			if (WinningResult != EUnrealAISecretStoreResult::Succeeded || WinningRevision == 0)
			{
				OutNewRevision = 0;
				OutError = WinningError;
				return WinningResult;
			}
			OutNewRevision = 0;
			OutError = MakeOAuthTestError(EUnrealAIErrorCategory::VersionMismatch,
										  EUnrealAIProviderAccessErrorCode::SecretRevisionConflict, true);
			return EUnrealAISecretStoreResult::Conflict;
		}
		if (bForceConflict.Load())
		{
			OutNewRevision = 0;
			OutError = MakeOAuthTestError(EUnrealAIErrorCategory::VersionMismatch,
										  EUnrealAIProviderAccessErrorCode::SecretRevisionConflict, true);
			return EUnrealAISecretStoreResult::Conflict;
		}
		return Inner->Store(Context, Handle, Value, ExpectedRevision, OutNewRevision, OutError);
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &Context,
									  const FUnrealAISecretHandle &Handle, const uint64 ExpectedRevision,
									  FUnrealAIProviderAccessError &OutError) override
	{
		return Inner->Delete(Context, Handle, ExpectedRevision, OutError);
	}

	TAtomic<int32> LoadCalls{0};
	TAtomic<int32> ReturnedLoads{0};
	TAtomic<int32> StoreCalls{0};
	TAtomic<bool> bForceConflict{false};
	TAtomic<bool> bCommitWinnerThenConflict{false};
	TAtomic<int32> ForcedLoadResult{-1};

  private:
	TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Inner;
	int32 LoadBarrierTarget = 0;
	FEvent *LoadBarrier = nullptr;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerFreshProtectedLeaseTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.FreshProtectedLeaseAndExactDestination",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerFreshProtectedLeaseTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeOAuthTestAccount(1);
	const FUnrealAISecretHandle Handle = MakeOAuthTestHandle(11);
	const FUnrealAIConnectionDescriptor Connection = MakeOAuthTestConnection(TEXT("tests.oauth.fresh"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 12, 0, 0), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 4);
	const FUnrealAIOAuthCredentialBinding Binding = MakeOAuthTestBinding(Account, Handle, true);
	TestTrue(TEXT("Fresh protected OAuth envelope is stored"),
				  StoreOAuthTestEnvelope(*Memory, Clock, Binding, Clock->UtcNow() + FTimespan::FromHours(1), true, true,
										 Error));
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = Memory;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock);
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh);
	TestTrue(TEXT("Exact OAuth binding registers"), Broker.RegisterBinding(Binding, Error));

	FUnrealAIOAuthCredentialBinding WrongProvider = Binding;
	WrongProvider.ProviderName = TEXT("tests.oauth.wrong");
	TestFalse(TEXT("Binding from a different provider is rejected"), Broker.RegisterBinding(WrongProvider, Error));

	FUnrealAICancellationSource Cancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> HandleOut;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Fresh OAuth resolve is admitted"),
				  Broker.StartResolve(MakeOAuthTestRequest(101, Connection.ConnectionAlias), Sink,
									  Cancellation.GetToken(), HandleOut, AccessError));
	FUnrealAICredentialResult Result;
	TestTrue(TEXT("Fresh OAuth resolve publishes a terminal"), Sink->WaitAndTake(Result));
	TestEqual(TEXT("Fresh OAuth resolve succeeds"), Result.Kind, EUnrealAICredentialResultKind::Succeeded);
	TestEqual(TEXT("Fresh access avoids refresh"), Refresh->Calls.Load(), 0);
	TestTrue(TEXT("Fresh resolve publishes one opaque context"), Result.AccessContext.IsValid());

	FUnrealAICredentialDestination WrongDestination = Connection.CredentialDestination;
	WrongDestination.Audience = TEXT("different-oauth-audience");
	FOAuthTestCredentialApplicator WrongApplicator(WrongDestination);
	TestFalse(TEXT("Destination mismatch exposes neither protected credential"),
				   Result.AccessContext->TryDispatch(WrongApplicator, Error));
	TestEqual(TEXT("Destination mismatch exposes no primary bytes"), WrongApplicator.PrimaryBytes, 0);
	TestEqual(TEXT("Destination mismatch exposes no routing bytes"), WrongApplicator.SecondaryBytes, 0);
	TestTrue(TEXT("Destination mismatch does not consume the protected lease"), Result.AccessContext->IsValid());

	FOAuthTestCredentialApplicator Applicator(Connection.CredentialDestination);
	TestTrue(TEXT("Exact destination consumes bearer and protected routing together"),
				  Result.AccessContext->TryDispatch(Applicator, Error));
	TestTrue(TEXT("Protected pair applicator was selected"), Applicator.bPairApplied);
	TestEqual(TEXT("Fresh bearer fixture has its exact byte count"), Applicator.PrimaryBytes, 15);
	TestEqual(TEXT("Protected routing fixture has its exact byte count"), Applicator.SecondaryBytes, 14);
	TestFalse(TEXT("OAuth access context is one-shot"), Result.AccessContext->TryDispatch(Applicator, Error));
	TestEqual(TEXT("Fresh resolve publishes exactly once"), Sink->GetCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerRefreshAndMissingMaterialTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.ExpiredRefreshAndRequiredMaterial",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerRefreshAndMissingMaterialTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAIAccessAccountId RefreshableAccount = MakeOAuthTestAccount(201);
	const FUnrealAIAccessAccountId MissingRefreshAccount = MakeOAuthTestAccount(211);
	const FUnrealAIAccessAccountId MissingRoutingAccount = MakeOAuthTestAccount(221);
	const FUnrealAIConnectionDescriptor RefreshableConnection =
		MakeOAuthTestConnection(TEXT("tests.oauth.expired_refreshable"), RefreshableAccount, Error);
	const FUnrealAIConnectionDescriptor MissingRefreshConnection =
		MakeOAuthTestConnection(TEXT("tests.oauth.expired_missing_refresh"), MissingRefreshAccount, Error);
	const FUnrealAIConnectionDescriptor MissingRoutingConnection =
		MakeOAuthTestConnection(TEXT("tests.oauth.missing_routing"), MissingRoutingAccount, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({RefreshableConnection, MissingRefreshConnection, MissingRoutingConnection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 13, 0, 0), 20.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 8);
	const FUnrealAIOAuthCredentialBinding RefreshableBinding =
		MakeOAuthTestBinding(RefreshableAccount, MakeOAuthTestHandle(301), false);
	const FUnrealAIOAuthCredentialBinding MissingRefreshBinding =
		MakeOAuthTestBinding(MissingRefreshAccount, MakeOAuthTestHandle(311), false);
	const FUnrealAIOAuthCredentialBinding MissingRoutingBinding =
		MakeOAuthTestBinding(MissingRoutingAccount, MakeOAuthTestHandle(321), true);
	const FDateTime OriginalExpiry = Clock->UtcNow() + FTimespan::FromSeconds(1);
	TestTrue(TEXT("Refreshable expiry fixture stores"),
				  StoreOAuthTestEnvelope(*Memory, Clock, RefreshableBinding, OriginalExpiry, true, false, Error));
	TestTrue(TEXT("Missing-refresh expiry fixture stores"),
				  StoreOAuthTestEnvelope(*Memory, Clock, MissingRefreshBinding, OriginalExpiry, false, false, Error));
	TestTrue(TEXT("Missing-routing fresh fixture stores"),
				  StoreOAuthTestEnvelope(*Memory, Clock, MissingRoutingBinding,
										 Clock->UtcNow() + FTimespan::FromHours(1), true, false, Error));
	MutableClock->Advance(FTimespan::FromSeconds(2));

	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = Memory;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock);
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh);
	TestTrue(TEXT("Refreshable binding registers"), Broker.RegisterBinding(RefreshableBinding, Error));
	TestTrue(TEXT("Missing-refresh binding registers"), Broker.RegisterBinding(MissingRefreshBinding, Error));
	TestTrue(TEXT("Missing-routing binding registers"), Broker.RegisterBinding(MissingRoutingBinding, Error));

	auto Resolve = [&Broker](const uint32 Seed, const FName Alias, FUnrealAICredentialResult &OutResult)
	{
		FUnrealAICancellationSource Cancellation;
		const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
		FUnrealAIProviderAccessError ErrorOut;
		return Broker.StartResolve(MakeOAuthTestRequest(Seed, Alias), Sink, Cancellation.GetToken(), RequestHandle,
								   ErrorOut) &&
			   Sink->WaitAndTake(OutResult);
	};

	FUnrealAICredentialResult RefreshableResult;
	TestTrue(TEXT("Expired envelope with refresh credential resolves"),
				  Resolve(401, RefreshableConnection.ConnectionAlias, RefreshableResult));
	TestEqual(TEXT("Expired envelope is refreshed instead of classified as corrupt"), RefreshableResult.Kind,
				   EUnrealAICredentialResultKind::Succeeded);
	TestEqual(TEXT("Expired access invokes one refresh"), Refresh->Calls.Load(), 1);

	FUnrealAICredentialResult MissingRefreshResult;
	TestTrue(TEXT("Expired missing-refresh envelope publishes a terminal"),
				  Resolve(411, MissingRefreshConnection.ConnectionAlias, MissingRefreshResult));
	TestEqual(TEXT("Missing refresh fails the resolve"), MissingRefreshResult.Kind,
				   EUnrealAICredentialResultKind::Failed);
	TestEqual(TEXT("Missing refresh requests reauthentication"), MissingRefreshResult.Error.Code,
				   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	TestEqual(TEXT("Missing refresh never calls provider refresh"), Refresh->Calls.Load(), 1);

	FUnrealAICredentialResult MissingRoutingResult;
	TestTrue(TEXT("Required-routing envelope publishes a terminal"),
				  Resolve(421, MissingRoutingConnection.ConnectionAlias, MissingRoutingResult));
	TestEqual(TEXT("Missing protected routing fails closed"), MissingRoutingResult.Kind,
				   EUnrealAICredentialResultKind::Failed);
	TestEqual(TEXT("Missing protected routing requests reauthentication"), MissingRoutingResult.Error.Code,
				   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	FUnrealAIProviderAccessError PublicQuarantine;
	TestTrue(TEXT("Missing refresh permanently quarantines the account"),
				  Broker.TryGetAccountQuarantine(MissingRefreshBinding.AuthProfileId, MissingRefreshBinding.AccountId,
												 PublicQuarantine));
	TestTrue(TEXT("Missing routing permanently quarantines the account"),
				  Broker.TryGetAccountQuarantine(MissingRoutingBinding.AuthProfileId, MissingRoutingBinding.AccountId,
												 PublicQuarantine));
	FUnrealAICancellationSource RejectedCancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> RejectedSink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RejectedHandle;
	FUnrealAIProviderAccessError RejectedError;
	TestFalse(TEXT("Missing-refresh quarantine rejects later resolve synchronously"),
				   Broker.StartResolve(MakeOAuthTestRequest(431, MissingRefreshConnection.ConnectionAlias),
									   RejectedSink, RejectedCancellation.GetToken(), RejectedHandle, RejectedError));
	TestEqual(TEXT("Permanent-material quarantine projects reauthentication"), RejectedError.Code,
				   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	FUnrealAICancellationSource ReplacementCancellation;
	TestFalse(TEXT("Stored record without refresh material cannot claim replacement"),
				   Broker.NotifyCredentialReplaced(MissingRefreshBinding.AuthProfileId, MissingRefreshBinding.AccountId,
												   1, 30.0, ReplacementCancellation.GetToken(), Error));
	TestFalse(TEXT("Stored record without required routing cannot claim replacement"),
				   Broker.NotifyCredentialReplaced(MissingRoutingBinding.AuthProfileId, MissingRoutingBinding.AccountId,
												   1, 30.0, ReplacementCancellation.GetToken(), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerPermanentStoreStateTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.PermanentStoreFaultsQuarantineTransientDoesNot",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerPermanentStoreStateTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAIAccessAccountId MissingAccount = MakeOAuthTestAccount(451);
	const FUnrealAIAccessAccountId CorruptAccount = MakeOAuthTestAccount(461);
	const FUnrealAIAccessAccountId TransientAccount = MakeOAuthTestAccount(471);
	const FUnrealAIConnectionDescriptor MissingConnection =
		MakeOAuthTestConnection(TEXT("tests.oauth.missing_record"), MissingAccount, Error);
	const FUnrealAIConnectionDescriptor CorruptConnection =
		MakeOAuthTestConnection(TEXT("tests.oauth.corrupt_record"), CorruptAccount, Error);
	const FUnrealAIConnectionDescriptor TransientConnection =
		MakeOAuthTestConnection(TEXT("tests.oauth.transient_record"), TransientAccount, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({MissingConnection, CorruptConnection, TransientConnection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 13, 30, 0), 25.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 8);
	const FUnrealAIOAuthCredentialBinding MissingBinding =
		MakeOAuthTestBinding(MissingAccount, MakeOAuthTestHandle(481), false);
	const FUnrealAIOAuthCredentialBinding CorruptBinding =
		MakeOAuthTestBinding(CorruptAccount, MakeOAuthTestHandle(491), false);
	const FUnrealAIOAuthCredentialBinding TransientBinding =
		MakeOAuthTestBinding(TransientAccount, MakeOAuthTestHandle(501), false);
	TestTrue(TEXT("Corrupt record fixture stores raw invalid bytes"),
				  StoreOAuthTestRaw(*Memory, Clock, CorruptBinding.SecretHandle, "not-an-envelope", Error));
	TestTrue(TEXT("Transient record fixture stores a valid envelope"),
				  StoreOAuthTestEnvelope(*Memory, Clock, TransientBinding, Clock->UtcNow() + FTimespan::FromHours(1),
										 true, false, Error));
	const TSharedRef<FOAuthTestStore, ESPMode::ThreadSafe> FaultStore =
		MakeShared<FOAuthTestStore, ESPMode::ThreadSafe>(Memory);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = FaultStore;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock);
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh);
	TestTrue(TEXT("Missing record binding registers"), Broker.RegisterBinding(MissingBinding, Error));
	TestTrue(TEXT("Corrupt record binding registers"), Broker.RegisterBinding(CorruptBinding, Error));
	TestTrue(TEXT("Transient record binding registers"), Broker.RegisterBinding(TransientBinding, Error));

	auto Resolve = [&Broker](const uint32 Seed, const FName Alias, FUnrealAICredentialResult &OutResult)
	{
		FUnrealAICancellationSource Cancellation;
		const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> Handle;
		FUnrealAIProviderAccessError AccessError;
		return Broker.StartResolve(MakeOAuthTestRequest(Seed, Alias), Sink, Cancellation.GetToken(), Handle,
								   AccessError) &&
			   Sink->WaitAndTake(OutResult);
	};

	FUnrealAICredentialResult MissingResult;
	TestTrue(TEXT("Missing record publishes a terminal"),
				  Resolve(511, MissingConnection.ConnectionAlias, MissingResult));
	TestEqual(TEXT("Missing record preserves typed failure"), MissingResult.Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretNotFound);
	FUnrealAIProviderAccessError PublicQuarantine;
	TestTrue(
		TEXT("Missing record quarantines"),
			 Broker.TryGetAccountQuarantine(MissingBinding.AuthProfileId, MissingBinding.AccountId, PublicQuarantine));

	FUnrealAICredentialResult CorruptResult;
	TestTrue(TEXT("Corrupt record publishes a terminal"),
				  Resolve(521, CorruptConnection.ConnectionAlias, CorruptResult));
	TestEqual(TEXT("Corrupt record preserves typed failure"), CorruptResult.Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
	TestTrue(
		TEXT("Corrupt record quarantines"),
			 Broker.TryGetAccountQuarantine(CorruptBinding.AuthProfileId, CorruptBinding.AccountId, PublicQuarantine));

	FaultStore->ForceNextLoad(EUnrealAISecretStoreResult::Locked);
	FUnrealAICredentialResult LockedResult;
	TestTrue(TEXT("Transient locked store publishes a terminal"),
				  Resolve(531, TransientConnection.ConnectionAlias, LockedResult));
	TestEqual(TEXT("Transient locked store remains retryable"), LockedResult.Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreLocked);
	TestFalse(TEXT("Transient locked store does not quarantine"),
				   Broker.TryGetAccountQuarantine(TransientBinding.AuthProfileId, TransientBinding.AccountId,
												  PublicQuarantine));
	FUnrealAICredentialResult RetryResult;
	TestTrue(TEXT("Resolve retries after transient store recovery"),
				  Resolve(541, TransientConnection.ConnectionAlias, RetryResult));
	TestEqual(TEXT("Transient recovery succeeds"), RetryResult.Kind, EUnrealAICredentialResultKind::Succeeded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerAdmissionLinearizationTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.QuarantineLinearizesBeforePhysicalAdmission",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerAdmissionLinearizationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeOAuthTestAccount(551);
	const FUnrealAIOAuthCredentialBinding Binding = MakeOAuthTestBinding(Account, MakeOAuthTestHandle(561), false);
	const FUnrealAIConnectionDescriptor Connection =
		MakeOAuthTestConnection(TEXT("tests.oauth.admission_linearization"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({Connection}, Error);
	const TSharedRef<FOAuthAdmissionGateClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FOAuthAdmissionGateClock, ESPMode::ThreadSafe>();
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 4);
	if (!TestTrue(TEXT("Admission-linearization envelope stores"),
					   StoreOAuthTestEnvelope(*Memory, Clock, Binding, Clock->UtcNow() + FTimespan::FromHours(1), true,
											  false, Error)))
	{
		return false;
	}
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = Memory;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock);
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh);
	if (!TestTrue(TEXT("Admission-linearization binding registers"), Broker.RegisterBinding(Binding, Error)))
	{
		return false;
	}

	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	FUnrealAICancellationSource Cancellation;
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError AdmissionError;
	MutableClock->BlockNextMonotonicSample();
	TFuture<bool> Admission =
		Async(EAsyncExecution::Thread,
			  [&Broker, &Connection, &Sink, &Cancellation, &Handle, &AdmissionError]()
			  {
				  return Broker.StartResolve(MakeOAuthTestRequest(571, Connection.ConnectionAlias), Sink,
											 Cancellation.GetToken(), Handle, AdmissionError);
			  });
	if (!TestTrue(TEXT("Resolve pauses after its first binding snapshot"), MutableClock->WaitUntilBlocked()))
	{
		MutableClock->Release();
		Admission.Get();
		return false;
	}
	Broker.QuarantineAccountForReauthentication(Binding.AuthProfileId, Binding.AccountId);
	MutableClock->Release();
	TestFalse(TEXT("Quarantine wins before physical admission"), Admission.Get());
	TestFalse(TEXT("Linearized quarantine returns no handle"), Handle.IsValid());
	TestEqual(TEXT("Linearized quarantine preserves reauthentication error"), AdmissionError.Code,
				   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	TestEqual(TEXT("Rejected stale snapshot creates no logical operation"), Broker.GetActiveOperationCount(), 0);
	TestEqual(TEXT("Rejected stale snapshot creates no physical operation"), Broker.GetPhysicalOperationCount(), 0);
	TestEqual(TEXT("Synchronous stale-snapshot rejection emits no terminal"), Sink->GetCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerSingleFlightTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.PerAccountRefreshSingleFlight",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerSingleFlightTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	constexpr int32 ResolveCount = 6;
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeOAuthTestAccount(501);
	const FUnrealAIOAuthCredentialBinding Binding = MakeOAuthTestBinding(Account, MakeOAuthTestHandle(511), false);
	const FUnrealAIConnectionDescriptor Connection =
		MakeOAuthTestConnection(TEXT("tests.oauth.single_flight"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 14, 0, 0), 30.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 4);
	TestTrue(TEXT("Single-flight envelope stores"),
				  StoreOAuthTestEnvelope(*Memory, Clock, Binding, Clock->UtcNow() + FTimespan::FromSeconds(30), true,
										 false, Error));
	const TSharedRef<FOAuthTestStore, ESPMode::ThreadSafe> CoordinatedStore =
		MakeShared<FOAuthTestStore, ESPMode::ThreadSafe>(Memory);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = CoordinatedStore;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock, true);
	FUnrealAIOAuthCredentialBrokerConfig Config;
	Config.MaxConcurrentOperations = ResolveCount;
	Config.RefreshSkewSeconds = 120.0;
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh, Config);
	TestTrue(TEXT("Single-flight binding registers"), Broker.RegisterBinding(Binding, Error));

	TArray<TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe>> Sinks;
	TArray<TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe>> Handles;
	TArray<TUniquePtr<FUnrealAICancellationSource>> Cancellations;
	auto StartResolve = [&Broker, &Connection, &Sinks, &Handles, &Cancellations, this](const int32 Index)
	{
		const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
		Cancellations.Add(MakeUnique<FUnrealAICancellationSource>());
		FUnrealAIProviderAccessError AccessError;
		TestTrue(TEXT("Concurrent resolve is admitted"),
			Broker.StartResolve(MakeOAuthTestRequest(601 + static_cast<uint32>(Index * 10), Connection.ConnectionAlias),
								Sink, Cancellations.Last()->GetToken(), RequestHandle, AccessError));
		Sinks.Add(Sink);
		Handles.Add(RequestHandle);
	};

	StartResolve(0);
	TestTrue(TEXT("Single refresh leader enters"), Refresh->WaitUntilEntered());
	TestTrue(TEXT("One account refresh flight remains active while provider is blocked"),
				  WaitForOAuthTestCondition([&Broker]() { return Broker.GetRefreshFlightCount() == 1; }));
	for (int32 Index = 1; Index < ResolveCount; ++Index)
	{
		StartResolve(Index);
	}
	TestTrue(TEXT("Every concurrent waiter loads the same frozen revision"),
				  WaitForOAuthTestCondition([&CoordinatedStore]()
											{ return CoordinatedStore->LoadCalls.Load() == ResolveCount; }));
	FPlatformProcess::SleepNoStats(0.05f);
	TestEqual(TEXT("Concurrent resolves call provider refresh exactly once"), Refresh->Calls.Load(), 1);
	Refresh->Release();

	for (const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> &Sink : Sinks)
	{
		FUnrealAICredentialResult Result;
		TestTrue(TEXT("Single-flight waiter publishes a terminal"), Sink->WaitAndTake(Result));
		TestEqual(TEXT("Single-flight waiter reloads the rotated envelope"), Result.Kind,
					   EUnrealAICredentialResultKind::Succeeded);
		TestEqual(TEXT("Single-flight waiter publishes exactly once"), Sink->GetCount(), 1);
	}
	TestEqual(TEXT("Refresh remains single-flight after settlement"), Refresh->Calls.Load(), 1);
	TestTrue(TEXT("Refresh flight is removed after all waiters settle"),
				  WaitForOAuthTestCondition(
					  [&Broker]()
					  { return Broker.GetRefreshFlightCount() == 0 && Broker.GetActiveOperationCount() == 0; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerRotationConflictTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.RefreshRotationCompareAndSwapConflict",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerRotationConflictTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeOAuthTestAccount(701);
	const FUnrealAIOAuthCredentialBinding Binding = MakeOAuthTestBinding(Account, MakeOAuthTestHandle(711), false);
	const FUnrealAIConnectionDescriptor Connection =
		MakeOAuthTestConnection(TEXT("tests.oauth.cas_conflict"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 15, 0, 0), 40.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 4);
	TestTrue(TEXT("CAS-conflict envelope stores"),
				  StoreOAuthTestEnvelope(*Memory, Clock, Binding, Clock->UtcNow() + FTimespan::FromSeconds(30), true,
										 false, Error));
	const TSharedRef<FOAuthTestStore, ESPMode::ThreadSafe> ConflictStore =
		MakeShared<FOAuthTestStore, ESPMode::ThreadSafe>(Memory);
	ConflictStore->bForceConflict.Store(true);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = ConflictStore;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock);
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh);
	TestTrue(TEXT("CAS-conflict binding registers"), Broker.RegisterBinding(Binding, Error));

	FUnrealAICancellationSource Cancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("CAS-conflict resolve is admitted"),
				  Broker.StartResolve(MakeOAuthTestRequest(801, Connection.ConnectionAlias), Sink,
									  Cancellation.GetToken(), RequestHandle, AccessError));
	FUnrealAICredentialResult Result;
	TestTrue(TEXT("CAS-conflict resolve publishes a terminal"), Sink->WaitAndTake(Result));
	TestEqual(TEXT("CAS conflict fails the rotation"), Result.Kind, EUnrealAICredentialResultKind::Failed);
	TestEqual(TEXT("CAS conflict preserves typed version category"), Result.Error.Category,
				   EUnrealAIErrorCategory::VersionMismatch);
	TestEqual(TEXT("CAS conflict preserves typed revision code"), Result.Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretRevisionConflict);
	TestTrue(TEXT("CAS conflict remains retryable"), Result.Error.bRetryable);
	TestEqual(TEXT("Provider refresh happened once before CAS"), Refresh->Calls.Load(), 1);
	uint64 Revision = 0;
	TestTrue(TEXT("Original atomic envelope remains loadable after conflict"),
				  LoadOAuthTestRevision(*Memory, Clock, Binding.SecretHandle, Revision));
	TestEqual(TEXT("Conflict does not overwrite the winning revision"), Revision, uint64(1));
	TestEqual(TEXT("CAS conflict publishes exactly once"), Sink->GetCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerRotationConflictWinnerTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.RefreshRotationAdoptsFreshCompareAndSwapWinner",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerRotationConflictWinnerTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeOAuthTestAccount(751);
	const FUnrealAIOAuthCredentialBinding Binding = MakeOAuthTestBinding(Account, MakeOAuthTestHandle(761), false);
	const FUnrealAIConnectionDescriptor Connection =
		MakeOAuthTestConnection(TEXT("tests.oauth.cas_winner"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 15, 30, 0), 45.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 4);
	TestTrue(TEXT("CAS-winner envelope stores"),
				  StoreOAuthTestEnvelope(*Memory, Clock, Binding, Clock->UtcNow() + FTimespan::FromSeconds(30), true,
										 false, Error));
	const TSharedRef<FOAuthTestStore, ESPMode::ThreadSafe> ConflictStore =
		MakeShared<FOAuthTestStore, ESPMode::ThreadSafe>(Memory);
	ConflictStore->bCommitWinnerThenConflict.Store(true);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = ConflictStore;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock);
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh);
	TestTrue(TEXT("CAS-winner binding registers"), Broker.RegisterBinding(Binding, Error));

	FUnrealAICancellationSource Cancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("CAS-winner resolve is admitted"),
				  Broker.StartResolve(MakeOAuthTestRequest(771, Connection.ConnectionAlias), Sink,
									  Cancellation.GetToken(), RequestHandle, AccessError));
	FUnrealAICredentialResult Result;
	TestTrue(TEXT("CAS-winner resolve publishes"), Sink->WaitAndTake(Result));
	TestEqual(TEXT("Fresh winning envelope satisfies the resolve"), Result.Kind,
				   EUnrealAICredentialResultKind::Succeeded);
	TestEqual(TEXT("Provider refresh remains exactly once"), Refresh->Calls.Load(), 1);
	TestEqual(TEXT("Winner conflict performs the initial load plus one exact reload"), ConflictStore->LoadCalls.Load(),
				   2);
	TestEqual(TEXT("Only one compare-and-swap store was attempted"), ConflictStore->StoreCalls.Load(), 1);
	uint64 Revision = 0;
	TestTrue(TEXT("Fresh winning envelope remains stored"),
				  LoadOAuthTestRevision(*Memory, Clock, Binding.SecretHandle, Revision));
	TestEqual(TEXT("Winning revision is adopted"), Revision, uint64(2));
	TestEqual(TEXT("Winner path publishes exactly once"), Sink->GetCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerLateTerminalTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.CancellationTimeoutAndLateRefreshTerminalOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerLateTerminalTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	auto RunLateTerminalCase = [this](const bool bTimeout, const uint32 Seed)
	{
		FString Error;
		const FUnrealAIAccessAccountId Account = MakeOAuthTestAccount(Seed);
		const FUnrealAIOAuthCredentialBinding Binding =
			MakeOAuthTestBinding(Account, MakeOAuthTestHandle(Seed + 10), false);
		const FUnrealAIConnectionDescriptor Connection = MakeOAuthTestConnection(
			bTimeout ? TEXT("tests.oauth.late_timeout") : TEXT("tests.oauth.late_cancel"), Account, Error);
		const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
			MakeOAuthTestConnections({Connection}, Error);
		const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
			MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 16, 0, 0), 50.0);
		const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
		const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
			MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 4);
		if (!TestTrue(TEXT("Late-terminal envelope stores"),
						   StoreOAuthTestEnvelope(*Memory, Clock, Binding, Clock->UtcNow() + FTimespan::FromSeconds(30),
												  true, false, Error)))
		{
			return false;
		}
		const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = Memory;
		const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
			MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock, true, true);
		FUnrealAIOAuthCredentialBrokerConfig BrokerConfig;
		BrokerConfig.MaxConcurrentOperations = 1;
		FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh, BrokerConfig);
		if (!TestTrue(TEXT("Late-terminal binding registers"), Broker.RegisterBinding(Binding, Error)))
		{
			return false;
		}

		FUnrealAICancellationSource Cancellation;
		TAtomic<int32> ActiveAtDelivery{-1};
		const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>(
				[&Broker, &ActiveAtDelivery]() { ActiveAtDelivery.Store(Broker.GetActiveOperationCount()); });
		TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
		FUnrealAIProviderAccessError AccessError;
		if (!TestTrue(TEXT("Late-terminal resolve is admitted"),
						   Broker.StartResolve(MakeOAuthTestRequest(Seed + 20, Connection.ConnectionAlias, 1.0f), Sink,
											   Cancellation.GetToken(), RequestHandle, AccessError)) ||
					  !TestTrue(TEXT("Late-terminal refresh enters"), Refresh->WaitUntilEntered()))
		{
			Refresh->Release();
			return false;
		}
		if (bTimeout)
		{
			MutableClock->Advance(FTimespan::FromSeconds(1));
		}
		else
		{
			RequestHandle->Cancel();
		}
		FUnrealAICredentialResult Result;
		if (!TestTrue(TEXT("Logical terminal settles before provider returns"), Sink->WaitAndTake(Result)))
		{
			Refresh->Release();
			return false;
		}
		TestEqual(TEXT("Logical terminal has exact kind"), Result.Kind,
					   bTimeout ? EUnrealAICredentialResultKind::TimedOut : EUnrealAICredentialResultKind::Cancelled);
		TestEqual(TEXT("Logical operation is removed before external terminal delivery"), ActiveAtDelivery.Load(), 0);
		TestEqual(TEXT("Logical terminal immediately releases the public active count"),
					   Broker.GetActiveOperationCount(), 0);
		TestEqual(TEXT("Blocked refresh remains physically fenced"), Broker.GetPhysicalOperationCount(), 1);
		FUnrealAICancellationSource OverlapCancellation;
		const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> OverlapSink =
			MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> OverlapHandle;
		TestFalse(TEXT("Physical capacity rejects overlap after logical terminal"),
					   Broker.StartResolve(MakeOAuthTestRequest(Seed + 30, Connection.ConnectionAlias), OverlapSink,
										   OverlapCancellation.GetToken(), OverlapHandle, AccessError));
		TestEqual(TEXT("Physical overlap rejection is retryable busy"), AccessError.Code,
					   EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity);
		TestFalse(TEXT("Physical overlap rejection creates no handle"), OverlapHandle.IsValid());
		Refresh->Release();
		TestTrue(TEXT("Late refresh reconciles bounded operation and flight state"),
					  WaitForOAuthTestCondition(
						  [&Broker]()
						  {
							  return Broker.GetActiveOperationCount() == 0 && Broker.GetPhysicalOperationCount() == 0 &&
									 Broker.GetRefreshFlightCount() == 0;
						  }));
		FPlatformProcess::SleepNoStats(0.02f);
		TestEqual(TEXT("Cancellation/timeout and late success publish exactly once"), Sink->GetCount(), 1);
		uint64 Revision = 0;
		TestTrue(TEXT("Late result leaves original envelope loadable"),
					  LoadOAuthTestRevision(*Memory, Clock, Binding.SecretHandle, Revision));
		TestEqual(TEXT("Late result cannot persist rotation"), Revision, uint64(1));
		return true;
	};

	TestTrue(TEXT("Cancellation late-result case passes"), RunLateTerminalCase(false, 901));
	TestTrue(TEXT("Timeout late-result case passes"), RunLateTerminalCase(true, 1001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthCredentialBrokerInvalidationShutdownTest,
								 "UnrealAI.Auth.OAuthCredentialBroker.InvalidationIsolationAndShutdown",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerInvalidationShutdownTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeOAuthTestAccount(1101);
	const FUnrealAIOAuthCredentialBinding Binding = MakeOAuthTestBinding(Account, MakeOAuthTestHandle(1111), false);
	const FUnrealAIConnectionDescriptor ConnectionA =
		MakeOAuthTestConnection(TEXT("tests.oauth.invalidate_a"), Account, Error);
	const FUnrealAIConnectionDescriptor ConnectionB =
		MakeOAuthTestConnection(TEXT("tests.oauth.invalidate_b"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({ConnectionA, ConnectionB}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 17, 0, 0), 60.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 4);
	TestTrue(TEXT("Invalidation envelope stores"),
				  StoreOAuthTestEnvelope(*Memory, Clock, Binding, Clock->UtcNow() + FTimespan::FromHours(1), true,
										 false, Error));
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = Memory;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock);
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh);
	TestTrue(TEXT("Invalidation binding registers"), Broker.RegisterBinding(Binding, Error));

	auto Resolve = [&Broker](const uint32 Seed, const FName Alias, FUnrealAICredentialResult &OutResult)
	{
		FUnrealAICancellationSource Cancellation;
		const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
		FUnrealAIProviderAccessError AccessError;
		return Broker.StartResolve(MakeOAuthTestRequest(Seed, Alias), Sink, Cancellation.GetToken(), RequestHandle,
								   AccessError) &&
			   Sink->WaitAndTake(OutResult);
	};

	FUnrealAICredentialResult ResultA;
	FUnrealAICredentialResult ResultB;
	TestTrue(TEXT("Connection A lease resolves"), Resolve(1201, ConnectionA.ConnectionAlias, ResultA));
	TestTrue(TEXT("Connection B lease resolves"), Resolve(1211, ConnectionB.ConnectionAlias, ResultB));
	Broker.InvalidateConnection(ConnectionA.ConnectionAlias);
	FOAuthTestCredentialApplicator ApplicatorA(ConnectionA.CredentialDestination);
	FOAuthTestCredentialApplicator ApplicatorB(ConnectionB.CredentialDestination);
	TestFalse(TEXT("Exact connection invalidation revokes connection A"),
				   ResultA.AccessContext->TryDispatch(ApplicatorA, Error));
	TestTrue(TEXT("Exact connection invalidation preserves sibling connection B"),
				  ResultB.AccessContext->TryDispatch(ApplicatorB, Error));

	FUnrealAICredentialResult AccountResult;
	TestTrue(TEXT("Post-connection-invalidation account lease resolves"),
				  Resolve(1221, ConnectionA.ConnectionAlias, AccountResult));
	Broker.InvalidateAccount(Binding.AuthProfileId, Binding.AccountId);
	FOAuthTestCredentialApplicator AccountApplicator(ConnectionA.CredentialDestination);
	TestFalse(TEXT("Account logout/invalidation revokes every lease generation"),
				   AccountResult.AccessContext->TryDispatch(AccountApplicator, Error));

	Broker.BeginShutdown();
	TestTrue(TEXT("Broker shutdown is monotonic"), Broker.IsShutdown());
	Broker.BeginShutdown();
	FUnrealAICancellationSource RejectedCancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> RejectedSink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RejectedHandle;
	FUnrealAIProviderAccessError AccessError;
	TestFalse(TEXT("Shutdown rejects later resolve synchronously"),
				   Broker.StartResolve(MakeOAuthTestRequest(1231, ConnectionA.ConnectionAlias), RejectedSink,
									   RejectedCancellation.GetToken(), RejectedHandle, AccessError));
	TestFalse(TEXT("Shutdown rejection returns no handle"), RejectedHandle.IsValid());
	TestEqual(TEXT("Shutdown rejection emits no asynchronous terminal"), RejectedSink->GetCount(), 0);

	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> BlockingShutdownRefresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock, true, true);
	FUnrealAIOAuthCredentialBroker ShutdownRaceBroker(Connections, Store, Clock, BlockingShutdownRefresh);
	TestTrue(TEXT("Shutdown-race binding registers"), ShutdownRaceBroker.RegisterBinding(Binding, Error));
	ShutdownRaceBroker.ForceRefreshAccount(Binding.AuthProfileId, Binding.AccountId);
	FUnrealAICancellationSource ShutdownRaceCancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> ShutdownRaceSink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> ShutdownRaceHandle;
	TestTrue(TEXT("Shutdown-race refresh resolve is admitted"),
				  ShutdownRaceBroker.StartResolve(MakeOAuthTestRequest(1241, ConnectionA.ConnectionAlias),
												  ShutdownRaceSink, ShutdownRaceCancellation.GetToken(),
												  ShutdownRaceHandle, AccessError));
	TestTrue(TEXT("Shutdown-race provider call enters"), BlockingShutdownRefresh->WaitUntilEntered());
	ShutdownRaceBroker.BeginShutdown();
	FUnrealAICredentialResult ShutdownRaceResult;
	TestTrue(TEXT("Shutdown settles the logical operation before provider return"),
				  ShutdownRaceSink->WaitAndTake(ShutdownRaceResult));
	TestEqual(TEXT("Shutdown publishes one cancelled terminal"), ShutdownRaceResult.Kind,
				   EUnrealAICredentialResultKind::Cancelled);
	BlockingShutdownRefresh->Release();
	TestTrue(TEXT("Late shutdown refresh reconciles operation and flight state"),
				  WaitForOAuthTestCondition(
					  [&ShutdownRaceBroker]() {
						  return ShutdownRaceBroker.GetActiveOperationCount() == 0 &&
								 ShutdownRaceBroker.GetRefreshFlightCount() == 0;
					  }));
	FPlatformProcess::SleepNoStats(0.02f);
	TestEqual(TEXT("Shutdown and late provider return publish exactly once"), ShutdownRaceSink->GetCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIOAuthCredentialBrokerRefreshQuarantineTest,
	"UnrealAI.Auth.OAuthCredentialBroker.NonRetryableRefreshQuarantinesUntilReauthentication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthCredentialBrokerRefreshQuarantineTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeOAuthTestAccount(1301);
	const FUnrealAIOAuthCredentialBinding Binding = MakeOAuthTestBinding(Account, MakeOAuthTestHandle(1311), false);
	const FUnrealAIConnectionDescriptor Connection =
		MakeOAuthTestConnection(TEXT("tests.oauth.refresh_quarantine"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeOAuthTestConnections({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 23, 18, 0, 0), 70.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(OAuthStoreName, 4);
	TestTrue(TEXT("Refresh-quarantine envelope stores"),
				  StoreOAuthTestEnvelope(*Memory, Clock, Binding, Clock->UtcNow() + FTimespan::FromSeconds(1), true,
										 false, Error));
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = Memory;
	const TSharedRef<FOAuthTestRefreshSource, ESPMode::ThreadSafe> Refresh =
		MakeShared<FOAuthTestRefreshSource, ESPMode::ThreadSafe>(Clock);
	Refresh->bSucceed.Store(false);
	Refresh->ForcedFailure = MakeOAuthTestError(EUnrealAIErrorCategory::PolicyDenied,
												EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied);
	FUnrealAIOAuthCredentialBroker Broker(Connections, Store, Clock, Refresh);
	TestTrue(TEXT("Refresh-quarantine binding registers"), Broker.RegisterBinding(Binding, Error));

	FUnrealAICancellationSource FirstCancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> FirstSink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> FirstHandle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("First entitlement-denied refresh is admitted"),
				  Broker.StartResolve(MakeOAuthTestRequest(1321, Connection.ConnectionAlias), FirstSink,
									  FirstCancellation.GetToken(), FirstHandle, AccessError));
	FUnrealAICredentialResult FirstResult;
	TestTrue(TEXT("First entitlement-denied refresh publishes"), FirstSink->WaitAndTake(FirstResult));
	TestEqual(TEXT("First entitlement denial is a failed credential terminal"), FirstResult.Kind,
				   EUnrealAICredentialResultKind::Failed);
	TestEqual(TEXT("First entitlement denial retains its closed code"), FirstResult.Error.Code,
				   EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied);
	TestEqual(TEXT("Provider refresh was attempted once"), Refresh->Calls.Load(), 1);

	FUnrealAICancellationSource QuarantinedCancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> QuarantinedSink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> QuarantinedHandle;
	TestFalse(TEXT("Quarantined account rejects the next resolve synchronously"),
				   Broker.StartResolve(MakeOAuthTestRequest(1331, Connection.ConnectionAlias), QuarantinedSink,
									   QuarantinedCancellation.GetToken(), QuarantinedHandle, AccessError));
	TestFalse(TEXT("Quarantined rejection creates no handle"), QuarantinedHandle.IsValid());
	TestEqual(TEXT("Quarantined rejection preserves entitlement code"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied);
	TestEqual(TEXT("Quarantine prevents a repeated provider refresh"), Refresh->Calls.Load(), 1);
	TestEqual(TEXT("Synchronous quarantine rejection emits no async terminal"), QuarantinedSink->GetCount(), 0);

	FUnrealAIProviderAccessError PublicQuarantine;
	TestTrue(TEXT("Public account status observes quarantine"),
				  Broker.TryGetAccountQuarantine(Binding.AuthProfileId, Binding.AccountId, PublicQuarantine));
	TestEqual(TEXT("Public quarantine hides provider entitlement detail"), PublicQuarantine.Code,
				   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	TestEqual(TEXT("Public quarantine uses a reauthentication category"), PublicQuarantine.Category,
				   EUnrealAIErrorCategory::NotAuthorized);

	Broker.InvalidateAccount(Binding.AuthProfileId, Binding.AccountId);
	Broker.InvalidateAuthProfile(Binding.AuthProfileId);
	FUnrealAICancellationSource StillQuarantinedCancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> StillQuarantinedSink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> StillQuarantinedHandle;
	TestFalse(TEXT("Ordinary account/profile invalidation cannot clear quarantine"),
				   Broker.StartResolve(MakeOAuthTestRequest(1336, Connection.ConnectionAlias), StillQuarantinedSink,
									   StillQuarantinedCancellation.GetToken(), StillQuarantinedHandle, AccessError));
	TestEqual(TEXT("Internal resolve retains the exact entitlement denial"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied);
	FUnrealAICancellationSource ReplacementCancellation;
	TestFalse(TEXT("Zero revision cannot claim credential replacement"),
				   Broker.NotifyCredentialReplaced(Binding.AuthProfileId, Binding.AccountId, 0, 30.0,
												   ReplacementCancellation.GetToken(), Error));
	TestFalse(TEXT("Unobserved nonzero revision cannot clear quarantine"),
				   Broker.NotifyCredentialReplaced(Binding.AuthProfileId, Binding.AccountId, 2, 30.0,
												   ReplacementCancellation.GetToken(), Error));
	TestTrue(TEXT("Rejected revision leaves public quarantine intact"),
				  Broker.TryGetAccountQuarantine(Binding.AuthProfileId, Binding.AccountId, PublicQuarantine));
	FUnrealAICancellationSource CancelledReplacement;
	CancelledReplacement.Cancel();
	TestFalse(TEXT("Cancelled verification cannot clear quarantine"),
				   Broker.NotifyCredentialReplaced(Binding.AuthProfileId, Binding.AccountId, 1, 30.0,
												   CancelledReplacement.GetToken(), Error));
	TestTrue(TEXT("Exact verified credential replacement clears quarantine"),
				  Broker.NotifyCredentialReplaced(Binding.AuthProfileId, Binding.AccountId, 1, 30.0,
												  ReplacementCancellation.GetToken(), Error));
	TestFalse(TEXT("Replacement removes public quarantine"),
				   Broker.TryGetAccountQuarantine(Binding.AuthProfileId, Binding.AccountId, PublicQuarantine));
	Refresh->bSucceed.Store(true);
	Refresh->ForcedFailure = {};
	FUnrealAICancellationSource ReauthenticatedCancellation;
	const TSharedRef<FOAuthTestCredentialSink, ESPMode::ThreadSafe> ReauthenticatedSink =
		MakeShared<FOAuthTestCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> ReauthenticatedHandle;
	TestTrue(TEXT("Resolve resumes after trusted account replacement"),
				  Broker.StartResolve(MakeOAuthTestRequest(1341, Connection.ConnectionAlias), ReauthenticatedSink,
									  ReauthenticatedCancellation.GetToken(), ReauthenticatedHandle, AccessError));
	FUnrealAICredentialResult ReauthenticatedResult;
	TestTrue(TEXT("Post-reauthentication resolve publishes"), ReauthenticatedSink->WaitAndTake(ReauthenticatedResult));
	TestEqual(TEXT("Post-reauthentication refresh succeeds"), ReauthenticatedResult.Kind,
				   EUnrealAICredentialResultKind::Succeeded);
	TestEqual(TEXT("Provider refresh resumes only after reauthentication"), Refresh->Calls.Load(), 2);
	return true;
}

#endif // defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS
