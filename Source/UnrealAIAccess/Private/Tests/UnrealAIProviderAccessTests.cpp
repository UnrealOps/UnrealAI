// Copyright EngineWorks. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"
#include "Misc/ScopeLock.h"

#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Auth/UnrealAIAccountAuthProviderRegistry.h"
#include "Auth/UnrealAIConnectionRegistry.h"
#include "Auth/UnrealAIMemorySecretStore.h"
#include "Auth/UnrealAIProviderAccess.h"

#include <limits>
#include <type_traits>

static_assert(!std::is_default_constructible_v<FUnrealAITrustedLocalAuthGesture>);
static_assert(!std::is_copy_constructible_v<FUnrealAITrustedLocalAuthGesture>);
static_assert(!std::is_copy_assignable_v<FUnrealAITrustedLocalAuthGesture>);
static_assert(!std::is_move_constructible_v<FUnrealAITrustedLocalAuthGesture>);
static_assert(!std::is_move_assignable_v<FUnrealAITrustedLocalAuthGesture>);

class FUnrealAIProviderAccessTestGestureAuthority final
{
  public:
	static bool StartSignIn(IUnrealAIAccountAuthProvider &Provider, const FUnrealAIInteractiveAuthRequest &Request,
							TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
							const FUnrealAICancellationToken &Cancellation,
							TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							FUnrealAIProviderAccessError &OutError)
	{
		const FUnrealAITrustedLocalAuthGesture Gesture;
		return Provider.StartSignIn(Gesture, Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
	}
};

namespace
{
bool MakeSecret(const TArray<uint8> &Fixture, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	TArray<uint8> Bytes = Fixture;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, OutError);
}

FUnrealAICredentialDestination BuildApiKeyDestination(FString &OutError)
{
	FUnrealAICredentialDestination Destination;
	Destination.ModelProviderName = TEXT("openai.compatible");
	Destination.AccountAuthProviderName = TEXT("project.secrets");
	Destination.AuthProfileId = TEXT("tests.api_key");
	Destination.AccountId.Value = FGuid(1, 2, 3, 4);
	Destination.TenantRealm = TEXT("tests.tenant_a");
	Destination.BillingPrincipalId.Value = FGuid(5, 6, 7, 8);
	Destination.PayerHandle = TEXT("tests.payer_a");
	Destination.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Destination.BillingMode = EUnrealAIBillingMode::ApiMetered;
	Destination.Audience = TEXT("openai-compatible-api");
	Destination.ConnectionRevision = 1;
	Destination.EndpointPolicyRevision = 1;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://api.example.com"), false, Destination.EndpointOrigin, OutError);
	return Destination;
}

FUnrealAICredentialDestination BuildSubscriptionDestination(FString &OutError)
{
	FUnrealAICredentialDestination Destination;
	Destination.ModelProviderName = TEXT("xai.grok");
	Destination.AccountAuthProviderName = TEXT("xai.auth");
	Destination.AuthProfileId = TEXT("tests.grok_subscription");
	Destination.AccountId.Value = FGuid(1, 2, 3, 4);
	Destination.TenantRealm = TEXT("tests.tenant_a");
	Destination.BillingPrincipalId.Value = FGuid(5, 6, 7, 8);
	Destination.PayerHandle = TEXT("tests.payer_a");
	Destination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Destination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Destination.Audience = TEXT("grok-subscription-inference");
	Destination.ConnectionRevision = 1;
	Destination.EndpointPolicyRevision = 1;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://subscription.xai.example"), false, Destination.EndpointOrigin,
										   OutError);
	return Destination;
}

FUnrealAICredentialDestination BuildOfflineDestination()
{
	FUnrealAICredentialDestination Destination;
	Destination.ModelProviderName = TEXT("local.deterministic");
	Destination.TenantRealm = TEXT("tests.local_tenant");
	Destination.AuthScheme = EUnrealAIAuthScheme::Anonymous;
	Destination.BillingMode = EUnrealAIBillingMode::Local;
	Destination.ConnectionRevision = 1;
	Destination.EndpointPolicyRevision = 1;
	return Destination;
}

FUnrealAIConnectionDescriptor BuildApiKeyConnection(const FString &Alias, FString &OutError)
{
	FUnrealAIConnectionDescriptor Descriptor;
	Descriptor.ConnectionAlias = FName(*Alias);
	Descriptor.EndpointProfileId = TEXT("tests.custom_endpoint");
	Descriptor.CredentialDestination = BuildApiKeyDestination(OutError);
	return Descriptor;
}

FUnrealAIProviderAccessError MakeSafeError(const EUnrealAIErrorCategory Category,
										   const EUnrealAIProviderAccessErrorCode Code)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	return Error;
}

/** Deterministically injects cancellation after a timeout check has begun but before the next commit check. */
class FCancelOnMonotonicReadClock final : public IUnrealAIClock
{
  public:
	explicit FCancelOnMonotonicReadClock(const FUnrealAICancellationSource &InCancellation)
		: Cancellation(InCancellation)
	{
	}

	FDateTime UtcNow() const override
	{
		return FDateTime(2026, 7, 20, 12, 0, 0);
	}

	double MonotonicSeconds() const override
	{
		FScopeLock Lock(&Mutex);
		++ReadCount;
		if (CancelAtRead > 0 && ReadCount == CancelAtRead)
		{
			Cancellation.Cancel(EUnrealAICancellationReason::Requested);
		}
		return 10.0;
	}

	double WorldSeconds() const
	{
		return 10.0;
	}

	void Arm(const int32 InCancelAtRead)
	{
		FScopeLock Lock(&Mutex);
		ReadCount = 0;
		CancelAtRead = InCancelAtRead;
	}

  private:
	FUnrealAICancellationSource Cancellation;
	mutable FCriticalSection Mutex;
	mutable int32 ReadCount = 0;
	int32 CancelAtRead = 0;
};

/** Returns the pre-deadline time for one selected read, then expires every subsequent commit check. */
class FExpireAfterMonotonicReadClock final : public IUnrealAIClock
{
  public:
	FDateTime UtcNow() const override
	{
		return FDateTime(2026, 7, 20, 12, 0, 0);
	}

	double MonotonicSeconds() const override
	{
		FScopeLock Lock(&Mutex);
		++ReadCount;
		const double Result = bExpired ? 10000.0 : 10.0;
		if (ExpireAfterRead > 0 && ReadCount == ExpireAfterRead)
		{
			bExpired = true;
		}
		return Result;
	}

	double WorldSeconds() const
	{
		return 10.0;
	}

	void Arm(const int32 InExpireAfterRead)
	{
		FScopeLock Lock(&Mutex);
		ReadCount = 0;
		ExpireAfterRead = InExpireAfterRead;
		bExpired = false;
	}

  private:
	mutable FCriticalSection Mutex;
	mutable int32 ReadCount = 0;
	mutable bool bExpired = false;
	int32 ExpireAfterRead = 0;
};

class FTestCredentialApplicator final : public IUnrealAICredentialApplicator
{
  public:
	explicit FTestCredentialApplicator(FUnrealAICredentialDestination InDestination)
		: Destination(MoveTemp(InDestination))
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	int32 ObservedCount = 0;
	uint32 ObservedSum = 0;
	bool bReject = false;
	bool bAnonymousDispatched = false;

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme Scheme, const TConstArrayView<uint8> Secret) override
	{
		if (bReject)
		{
			return false;
		}
		if (Scheme == EUnrealAIAuthScheme::Invalid)
		{
			return false;
		}
		ObservedCount = Secret.Num();
		for (const uint8 Byte : Secret)
		{
			ObservedSum += Byte;
		}
		return true;
	}
	bool DispatchWithoutCredential() override
	{
		bAnonymousDispatched = true;
		return !bReject;
	}

  private:
	FUnrealAICredentialDestination Destination;
};

class FProtectedCredentialApplicator final : public IUnrealAICredentialApplicator
{
  public:
	explicit FProtectedCredentialApplicator(FUnrealAICredentialDestination InDestination)
		: Destination(MoveTemp(InDestination))
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	int32 PairApplyCount = 0;
	int32 PrimaryCount = 0;
	int32 SecondaryCount = 0;
	uint32 PrimarySum = 0;
	uint32 SecondarySum = 0;
	bool bRejectPair = false;

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme, const TConstArrayView<uint8>) override
	{
		return false;
	}

	bool ApplyCredentialAndProtectedSecondaryAndDispatch(const EUnrealAIAuthScheme Scheme,
														 const TConstArrayView<uint8> Secret,
														 const TConstArrayView<uint8> ProtectedSecondary) override
	{
		++PairApplyCount;
		if (bRejectPair || Scheme != EUnrealAIAuthScheme::OAuthBearer || Secret.IsEmpty() ||
			ProtectedSecondary.IsEmpty())
		{
			return false;
		}
		PrimaryCount = Secret.Num();
		SecondaryCount = ProtectedSecondary.Num();
		for (const uint8 Byte : Secret)
		{
			PrimarySum += Byte;
		}
		for (const uint8 Byte : ProtectedSecondary)
		{
			SecondarySum += Byte;
		}
		return true;
	}

	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
};

class FReentrantCredentialApplicator final : public IUnrealAICredentialApplicator
{
  public:
	FReentrantCredentialApplicator(FUnrealAICredentialDestination InDestination, FUnrealAICredentialLease &InLease)
		: Destination(MoveTemp(InDestination)), Lease(InLease)
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	int32 ApplyCount = 0;
	int32 ObservedCount = 0;
	bool bInnerApplySucceeded = true;

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme, const TConstArrayView<uint8> Secret) override
	{
		++ApplyCount;
		if (!bAttemptedReentry)
		{
			bAttemptedReentry = true;
			FString InnerError;
			bInnerApplySucceeded = Lease.TryApplyTo(*this, InnerError);
		}
		ObservedCount += Secret.Num();
		return true;
	}
	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
	FUnrealAICredentialLease &Lease;
	bool bAttemptedReentry = false;
};

class FReentrantInvalidationApplicator final : public IUnrealAICredentialApplicator
{
  public:
	FReentrantInvalidationApplicator(FUnrealAICredentialDestination InDestination,
									 FUnrealAICredentialFreshnessSource &InFreshness)
		: Destination(MoveTemp(InDestination)), Freshness(InFreshness)
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	bool bReentrantInvalidationSucceeded = true;
	bool bReentrantTokenCurrent = true;
	bool bReentrantSourceReportsShutdown = false;
	int32 ObservedCount = 0;

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme, const TConstArrayView<uint8> Secret) override
	{
		bReentrantTokenCurrent = Freshness.GetToken().IsCurrent();
		bReentrantSourceReportsShutdown = Freshness.IsShutdown();
		bReentrantInvalidationSucceeded = Freshness.Invalidate();
		ObservedCount = Secret.Num();
		return true;
	}
	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
	FUnrealAICredentialFreshnessSource &Freshness;
};

class FBlockingCredentialApplicator final : public IUnrealAICredentialApplicator
{
  public:
	explicit FBlockingCredentialApplicator(FUnrealAICredentialDestination InDestination)
		: Destination(MoveTemp(InDestination)), Entered(FPlatformProcess::GetSynchEventFromPool(false)),
		  ReleaseEvent(FPlatformProcess::GetSynchEventFromPool(false))
	{
	}
	~FBlockingCredentialApplicator() override
	{
		ReleaseEvent->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseEvent);
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}
	bool WaitUntilEntered(const uint32 TimeoutMilliseconds) const
	{
		return Entered->Wait(TimeoutMilliseconds);
	}
	void Release()
	{
		ReleaseEvent->Trigger();
	}

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme, const TConstArrayView<uint8>) override
	{
		Entered->Trigger();
		return ReleaseEvent->Wait(5000);
	}
	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
	FEvent *Entered = nullptr;
	FEvent *ReleaseEvent = nullptr;
};

class FBlockingAnonymousApplicator final : public IUnrealAICredentialApplicator
{
  public:
	explicit FBlockingAnonymousApplicator(FUnrealAICredentialDestination InDestination)
		: Destination(MoveTemp(InDestination)), Entered(FPlatformProcess::GetSynchEventFromPool(false)),
		  ReleaseEvent(FPlatformProcess::GetSynchEventFromPool(false))
	{
	}
	~FBlockingAnonymousApplicator() override
	{
		ReleaseEvent->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		FPlatformProcess::ReturnSynchEventToPool(ReleaseEvent);
	}
	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}
	bool WaitUntilEntered(const uint32 TimeoutMilliseconds) const
	{
		return Entered->Wait(TimeoutMilliseconds);
	}
	void Release()
	{
		ReleaseEvent->Trigger();
	}

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme, const TConstArrayView<uint8>) override
	{
		return false;
	}
	bool DispatchWithoutCredential() override
	{
		Entered->Trigger();
		return ReleaseEvent->Wait(5000);
	}

  private:
	FUnrealAICredentialDestination Destination;
	FEvent *Entered = nullptr;
	FEvent *ReleaseEvent = nullptr;
};

class FDiscardingAuthEventSink final : public IUnrealAIAuthEventSink
{
  public:
	void EnqueueAuthEvent(FUnrealAIAuthEvent &&) override
	{
		++Count;
	}
	int32 Count = 0;
};

class FTestAccountAuthProvider final : public IUnrealAIAccountAuthProvider
{
  public:
	explicit FTestAccountAuthProvider(
		const FName InProviderName = TEXT("xai.auth"),
			const EUnrealAIProviderAccessAvailability InAvailability = EUnrealAIProviderAccessAvailability::Available,
			const EUnrealAIAuthScheme InAuthScheme = EUnrealAIAuthScheme::OAuthBearer,
			const EUnrealAIBillingMode InBillingMode = EUnrealAIBillingMode::SubscriptionQuota,
			const bool bInAdvertiseCapabilitiesWhenGated = false)
		: ProviderName(InProviderName),
		Availability(InAvailability), AuthScheme(InAuthScheme), BillingMode(InBillingMode),
		bAdvertiseCapabilitiesWhenGated(bInAdvertiseCapabilitiesWhenGated)
	{
	}
	FName GetProviderName() const override
	{
		return ProviderName;
	}
	FUnrealAIAccountAuthCapabilities DescribeCapabilities() const override
	{
		FUnrealAIAccountAuthCapabilities Capabilities;
		if (AuthScheme == EUnrealAIAuthScheme::OAuthBearer &&
			(bAdvertiseCapabilitiesWhenGated || (Availability != EUnrealAIProviderAccessAvailability::PartnerGated &&
												 Availability != EUnrealAIProviderAccessAvailability::Unsupported)))
		{
			Capabilities.bBrowserPkce = true;
			Capabilities.bDeviceCode = true;
		}
		return Capabilities;
	}
	FUnrealAIProviderAccessDescriptor DescribeAccess() const override
	{
		FUnrealAIProviderAccessDescriptor Access;
		Access.ModelProviderName = ProviderName == TEXT("openai.auth") ? TEXT("openai.subscription") : TEXT("xai.grok");
		Access.AccountAuthProviderName = ProviderName;
		Access.AuthScheme = AuthScheme;
		Access.BillingMode = BillingMode;
		Access.Availability = Availability;
		Access.SupportClassification = EUnrealAIProviderAccessSupportClassification::Supported;
		return Access;
	}
	FUnrealAIAccountStatus GetStatus(const FName AuthProfileId, const FUnrealAIAccessAccountId &) const override
	{
		FUnrealAIAccountStatus Status;
		Status.ProviderName = GetProviderName();
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
		OutError = MakeSafeError(EUnrealAIErrorCategory::UnsupportedCapability,
								 Availability == EUnrealAIProviderAccessAvailability::PartnerGated
									 ? EUnrealAIProviderAccessErrorCode::PartnerGated
									 : EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
		return false;
	}
	bool StartSignOut(const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIAccountAuthRequest &,
					  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe>, const FUnrealAICancellationToken &,
					  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError) override
	{
		OutHandle.Reset();
		OutError = MakeSafeError(EUnrealAIErrorCategory::UnsupportedCapability,
								 EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
		return false;
	}

  private:
	FName ProviderName;
	EUnrealAIProviderAccessAvailability Availability = EUnrealAIProviderAccessAvailability::Available;
	EUnrealAIAuthScheme AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	EUnrealAIBillingMode BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	bool bAdvertiseCapabilitiesWhenGated = false;
};

class FTestCredentialResultSink final : public IUnrealAICredentialResultSink
{
  public:
	void EnqueueCredentialResult(FUnrealAICredentialResult &&Result) override
	{
		FScopeLock Lock(&Mutex);
		++Count;
		LastKind = Result.Kind;
	}
	int32 GetCount() const
	{
		FScopeLock Lock(&Mutex);
		return Count;
	}
	EUnrealAICredentialResultKind GetLastKind() const
	{
		FScopeLock Lock(&Mutex);
		return LastKind;
	}

  private:
	mutable FCriticalSection Mutex;
	int32 Count = 0;
	EUnrealAICredentialResultKind LastKind = EUnrealAICredentialResultKind::Invalid;
};

class FTestCredentialOperationState final
{
  public:
	FTestCredentialOperationState(const FUnrealAIRequestId InRequestId,
								  TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> InSink)
		: RequestId(InRequestId), Sink(MoveTemp(InSink))
	{
	}

	bool TrySettle(const EUnrealAICredentialResultKind Kind)
	{
		EUnrealAITerminalKind TerminalKind = EUnrealAITerminalKind::Failed;
		if (Kind == EUnrealAICredentialResultKind::Cancelled)
		{
			TerminalKind = EUnrealAITerminalKind::Cancelled;
		}
		else if (Kind == EUnrealAICredentialResultKind::TimedOut)
		{
			TerminalKind = EUnrealAITerminalKind::TimedOut;
		}
		if (!Terminal.TryComplete(TerminalKind))
		{
			return false;
		}
		FUnrealAICredentialResult Result;
		Result.RequestId = RequestId;
		Result.Kind = Kind;
		if (Kind == EUnrealAICredentialResultKind::Cancelled)
		{
			Result.Error =
				MakeSafeError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::CredentialCancelled);
		}
		else if (Kind == EUnrealAICredentialResultKind::TimedOut)
		{
			Result.Error =
				MakeSafeError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::CredentialTimedOut);
		}
		else
		{
			Result.Kind = EUnrealAICredentialResultKind::Failed;
			Result.Error =
				MakeSafeError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed);
		}
		Sink->EnqueueCredentialResult(MoveTemp(Result));
		return true;
	}

  private:
	FUnrealAIRequestId RequestId;
	TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink;
	FUnrealAITerminalGuard Terminal;
};

class FTestCredentialRequestHandle final : public IUnrealAICredentialRequestHandle
{
  public:
	FTestCredentialRequestHandle(const FUnrealAIRequestId InRequestId,
								 TSharedRef<FTestCredentialOperationState, ESPMode::ThreadSafe> InState)
		: RequestId(InRequestId), State(MoveTemp(InState))
	{
	}
	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}
	void Cancel() override
	{
		State->TrySettle(EUnrealAICredentialResultKind::Cancelled);
	}

  private:
	FUnrealAIRequestId RequestId;
	TSharedRef<FTestCredentialOperationState, ESPMode::ThreadSafe> State;
};

class FTestCredentialBroker final : public IUnrealAICredentialBroker
{
  public:
	bool StartResolve(const FUnrealAICredentialRequest &Request,
					  TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError) override
	{
		OutHandle.Reset();
		OutError = FUnrealAIProviderAccessError{};
		FString ShapeError;
		if (!Request.ValidateShape(ShapeError))
		{
			OutError = MakeSafeError(EUnrealAIErrorCategory::InvalidArgument,
									 EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError =
				MakeSafeError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::CredentialCancelled);
			return false;
		}
		const TSharedRef<FTestCredentialOperationState, ESPMode::ThreadSafe> State =
			MakeShared<FTestCredentialOperationState, ESPMode::ThreadSafe>(Request.RequestId, Sink);
		LastState = State;
		OutHandle = MakeShared<FTestCredentialRequestHandle, ESPMode::ThreadSafe>(Request.RequestId, State);
		return true;
	}

	TSharedPtr<FTestCredentialOperationState, ESPMode::ThreadSafe> GetLastState() const
	{
		return LastState;
	}
	void InvalidateAccount(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) override
	{
		LastInvalidatedAuthProfile = AuthProfileId;
		LastInvalidatedAccount = AccountId;
	}
	void InvalidateAuthProfile(const FName AuthProfileId) override
	{
		LastInvalidatedAuthProfile = AuthProfileId;
	}
	void InvalidateConnection(const FName ConnectionAlias) override
	{
		LastInvalidatedConnection = ConnectionAlias;
	}
	void BeginShutdown() override
	{
		bShutdown = true;
	}

  private:
	TSharedPtr<FTestCredentialOperationState, ESPMode::ThreadSafe> LastState;
	FName LastInvalidatedAuthProfile;
	FUnrealAIAccessAccountId LastInvalidatedAccount;
	FName LastInvalidatedConnection;
	bool bShutdown = false;
};

TSharedRef<const FUnrealAIEndpointProfileRegistrySnapshot, ESPMode::ThreadSafe>
BuildApiEndpointProfiles(FString &OutError)
{
	const FUnrealAICredentialDestination Destination = BuildApiKeyDestination(OutError);
	FUnrealAIEndpointProfileRegistry Registry;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Registered;
	Registry.RegisterCustomApiEndpoint(TEXT("tests.custom_endpoint"), Destination.ModelProviderName,
											Destination.EndpointOrigin, Destination.Audience,
											Destination.EndpointPolicyRevision, Registered, OutError);
	return Registry.CreateSnapshot();
}
} // namespace

static_assert(!std::is_copy_constructible_v<FUnrealAISecretValue>);
static_assert(!std::is_copy_assignable_v<FUnrealAISecretValue>);
static_assert(std::is_move_constructible_v<FUnrealAISecretValue>);
static_assert(!std::is_copy_constructible_v<FUnrealAICredentialLease>);
static_assert(std::is_move_constructible_v<FUnrealAICredentialLease>);

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIEndpointOriginPolicyTest,
								 "UnrealAI.Auth.EndpointOriginCanonicalizationAndPolicy",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIEndpointOriginPolicyTest::RunTest(const FString &Parameters)
{
	FString Error;
	FUnrealAIEndpointOrigin Origin;
	TestTrue(TEXT("Secure origin parses"),
				  FUnrealAIEndpointOrigin::TryParse(TEXT("HTTPS://API.Example.COM:443/"), false, Origin, Error));
	TestEqual(TEXT("Scheme, host, default port, and slash canonicalize"), Origin.ToString(),
				   FString(TEXT("https://api.example.com")));
	TestTrue(TEXT("Canonical secure origin reports secure"), Origin.IsSecure());
	for (const TCHAR *LoopbackOriginText :
		 {TEXT("https://localhost"), TEXT("https://127.0.0.1"), TEXT("https://127.0.0.2"), TEXT("https://[::1]")})
	{
		FUnrealAIEndpointOrigin SecureLoopback;
		TestTrue(FString::Printf(TEXT("Secure loopback origin '%s' parses"), LoopbackOriginText),
								 FUnrealAIEndpointOrigin::TryParse(LoopbackOriginText, false, SecureLoopback, Error));
		TestTrue(FString::Printf(TEXT("Secure loopback origin '%s' retains loopback provenance"), LoopbackOriginText),
								 SecureLoopback.HasLoopbackHost());
		TestFalse(
			FString::Printf(TEXT("Secure loopback origin '%s' is not plaintext-development-only"), LoopbackOriginText),
							SecureLoopback.IsLoopbackDevelopmentOnly());
	}

	for (const TCHAR *InvalidOrigin :
		 {TEXT("https://user@api.example.com"), TEXT("https://api.example.com/v1"),
													 TEXT("https://api.example.com?key=value"),
														  TEXT("https://api.example.com#fragment"),
															   TEXT("ftp://api.example.com"),
																	TEXT("https://api.example.com:0"),
																		 TEXT("https://api.example.com:"),
																			  TEXT("https://-bad.example"),
																				   TEXT("https://bad-.example"),
																						TEXT("https://127.000.0.1"),
																							 TEXT("https://[:::]")})
	{
		TestFalse(FString::Printf(TEXT("Origin '%s' is rejected"), InvalidOrigin),
								  FUnrealAIEndpointOrigin::TryParse(InvalidOrigin, false, Origin, Error));
	}

	TestFalse(TEXT("Remote plaintext origin is rejected even in development mode"),
				   FUnrealAIEndpointOrigin::TryParse(TEXT("http://api.example.com"), true, Origin, Error));
	TestFalse(TEXT("Loopback plaintext origin requires explicit development opt-in"),
				   FUnrealAIEndpointOrigin::TryParse(TEXT("http://127.0.0.1:8080"), false, Origin, Error));
	TestTrue(TEXT("Explicit loopback development origin parses"),
				  FUnrealAIEndpointOrigin::TryParse(TEXT("http://127.0.0.1:8080/"), true, Origin, Error));
	TestEqual(TEXT("Non-default loopback port is retained"), Origin.ToString(), FString(TEXT("http://127.0.0.1:8080")));
	TestTrue(TEXT("Parsed plaintext origin retains development-only provenance"), Origin.IsLoopbackDevelopmentOnly());
	TestTrue(TEXT("Pure admission policy allows loopback development outside Shipping"),
				  FUnrealAIEndpointProfileRegistry::IsProfileClassAllowedInBuild(
					  EUnrealAIEndpointProfileClass::LocalLoopbackDevelopment, false));
	TestFalse(TEXT("Pure admission policy rejects loopback development in Shipping"),
				   FUnrealAIEndpointProfileRegistry::IsProfileClassAllowedInBuild(
					   EUnrealAIEndpointProfileClass::LocalLoopbackDevelopment, true));
	FUnrealAIEndpointProfileRegistry LoopbackProfiles;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> RegisteredLoopback;
	TestTrue(TEXT("Non-Shipping endpoint admission retains loopback development provenance"),
				  LoopbackProfiles.RegisterCustomApiEndpoint(
					  TEXT("tests.loopback"), TEXT("openai.compatible"), Origin,
												   TEXT("local-openai-compatible"), 1, RegisteredLoopback, Error));
	TestTrue(TEXT("Registered loopback endpoint is available"), RegisteredLoopback.IsValid());
	TestEqual(TEXT("Registered loopback endpoint remains development-only"), RegisteredLoopback->GetProfileClass(),
				   EUnrealAIEndpointProfileClass::LocalLoopbackDevelopment);
	const FString OversizedLabel = FString::ChrN(64, TEXT('a')) + TEXT(".example");
	TestFalse(TEXT("DNS label over 63 bytes is rejected"),
				   FUnrealAIEndpointOrigin::TryParse(TEXT("https://") + OversizedLabel, false, Origin, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAISecretValueContractTest, "UnrealAI.Auth.SecretValueMoveOnlyAndRedacted",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISecretValueContractTest::RunTest(const FString &Parameters)
{
	FString Error;
	FUnrealAISecretValue Secret;
	const TArray<uint8> Fixture{0x73, 0x65, 0x63, 0x72, 0x65, 0x74};
	TestTrue(TEXT("Bounded secret fixture constructs"), MakeSecret(Fixture, Secret, Error));
	TestTrue(TEXT("Constructed secret is set"), Secret.IsSet());
	TestEqual(TEXT("Secret retains exact byte count"), Secret.Num(), Fixture.Num());
	TestFalse(TEXT("Redacted display excludes plaintext fixture"),
				   Secret.GetRedactedDisplay().Contains(TEXT("secret"), ESearchCase::CaseSensitive));

	FUnrealAISecretValue Moved = MoveTemp(Secret);
	TestFalse(TEXT("Move clears source ownership"), Secret.IsSet());
	TestTrue(TEXT("Move transfers destination ownership"), Moved.IsSet());
	Moved.Reset();
	TestFalse(TEXT("Reset clears destination ownership"), Moved.IsSet());

	TArray<uint8> Empty;
	TestFalse(TEXT("Empty secret fails closed"), FUnrealAISecretValue::TryCreate(MoveTemp(Empty), Moved, Error));

	TArray<uint8> Maximum;
	Maximum.Init(0x5a, FUnrealAISecretValue::MaxSecretBytes);
	TestTrue(TEXT("Secret exact logical bound is accepted"),
				  FUnrealAISecretValue::TryCreate(MoveTemp(Maximum), Moved, Error));
	Moved.Reset();
	TArray<uint8> Oversized;
	Oversized.Init(0x5a, FUnrealAISecretValue::MaxSecretBytes + 1);
	TestFalse(TEXT("Secret logical bound plus one is rejected and output remains clear"),
				   FUnrealAISecretValue::TryCreate(MoveTemp(Oversized), Moved, Error));
	TestFalse(TEXT("Failed secret construction leaves no previous value"), Moved.IsSet());
	TArray<uint8> OverAllocated{0x5a};
	OverAllocated.Reserve(FUnrealAISecretValue::MaxSecretBytes * 2 + 1);
	TestFalse(TEXT("Excessive physical secret allocation is rejected"),
				   FUnrealAISecretValue::TryCreate(MoveTemp(OverAllocated), Moved, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAICredentialLeaseBindingTest,
								 "UnrealAI.Auth.CredentialLeaseExactDestinationBinding",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAICredentialLeaseBindingTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAICredentialDestination Binding = BuildSubscriptionDestination(Error);
	TestTrue(TEXT("Subscription destination validates"), Binding.ValidateShape(Error));
	const TArray<uint8> Fixture{1, 2, 3, 4, 5};
	const FDateTime Now(2026, 7, 20, 12, 0, 0);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(Now, 100.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	FUnrealAICredentialFreshnessSource Freshness;
	auto MakeBoundLease = [&Binding, &Clock, &Freshness, &Now, &Fixture, &Error](FUnrealAICredentialLease &OutLease)
	{
		FUnrealAISecretValue LocalSecret;
		return MakeSecret(Fixture, LocalSecret, Error) &&
			   FUnrealAICredentialLease::TryCreate(Binding, Clock, Freshness.GetToken(), 300.0,
												   Now + FTimespan::FromMinutes(5), MoveTemp(LocalSecret), OutLease,
												   Error);
	};
	FUnrealAICredentialLease Lease;
	TestTrue(TEXT("Origin-bound bearer lease constructs"), MakeBoundLease(Lease));
	TestTrue(TEXT("Constructed lease is valid"), Lease.IsValid());
	TestTrue(TEXT("Lease display remains redacted"), Lease.GetRedactedDisplay().StartsWith(TEXT("<redacted:")));

	FTestCredentialApplicator ExactApplicator(Binding);
	TestTrue(TEXT("Exact transport destination applies synchronously"), Lease.TryApplyTo(ExactApplicator, Error));
	TestEqual(TEXT("Transport observed exact size"), ExactApplicator.ObservedCount, Fixture.Num());
	TestEqual(TEXT("Transport observed exact contents"), ExactApplicator.ObservedSum, uint32(15));
	TestFalse(TEXT("Successful transport application consumes the lease"), Lease.IsValid());
	TestFalse(TEXT("Consumed lease cannot be replayed"), Lease.TryApplyTo(ExactApplicator, Error));
	FUnrealAICredentialLease RejectedLease;
	TestTrue(TEXT("Transport-rejection fixture lease constructs"), MakeBoundLease(RejectedLease));
	FTestCredentialApplicator RejectingApplicator(Binding);
	RejectingApplicator.bReject = true;
	TestFalse(TEXT("Transport rejection is surfaced"), RejectedLease.TryApplyTo(RejectingApplicator, Error));
	TestFalse(TEXT("Transport rejection still consumes the exposed lease"), RejectedLease.IsValid());
	FUnrealAICredentialLease ReentrantLease;
	TestTrue(TEXT("Reentrant fixture lease constructs"), MakeBoundLease(ReentrantLease));
	FReentrantCredentialApplicator ReentrantApplicator(Binding, ReentrantLease);
	TestTrue(TEXT("Outer one-shot transport application succeeds"),
				  ReentrantLease.TryApplyTo(ReentrantApplicator, Error));
	TestFalse(TEXT("Reentrant application cannot replay the lease"), ReentrantApplicator.bInnerApplySucceeded);
	TestEqual(TEXT("Reentrant transport sees only one callback"), ReentrantApplicator.ApplyCount, 1);
	TestEqual(TEXT("Outer callback retains valid one-shot bytes"), ReentrantApplicator.ObservedCount, Fixture.Num());
	FUnrealAICredentialFreshnessSource ReentrantFreshness;
	FUnrealAISecretValue ReentrantInvalidationSecret;
	TestTrue(TEXT("Reentrant invalidation fixture secret constructs"),
				  MakeSecret(Fixture, ReentrantInvalidationSecret, Error));
	FUnrealAICredentialLease ReentrantInvalidationLease;
	TestTrue(TEXT("Reentrant invalidation fixture lease constructs"),
				  FUnrealAICredentialLease::TryCreate(
					  Binding, Clock, ReentrantFreshness.GetToken(), 300.0, Now + FTimespan::FromMinutes(5),
					  MoveTemp(ReentrantInvalidationSecret), ReentrantInvalidationLease, Error));
	FReentrantInvalidationApplicator ReentrantInvalidationApplicator(Binding, ReentrantFreshness);
	TestTrue(TEXT("Dispatch completes without a reentrant freshness deadlock"),
				  ReentrantInvalidationLease.TryApplyTo(ReentrantInvalidationApplicator, Error));
	TestFalse(TEXT("Reentrant invalidation is rejected for deferred control-path handling"),
				   ReentrantInvalidationApplicator.bReentrantInvalidationSucceeded);
	TestFalse(TEXT("Reentrant token acquisition fails closed without recursive read locking"),
				   ReentrantInvalidationApplicator.bReentrantTokenCurrent);
	TestTrue(TEXT("Reentrant source inspection fails closed without recursive read locking"),
				  ReentrantInvalidationApplicator.bReentrantSourceReportsShutdown);
	TestEqual(TEXT("Guarded dispatch still admits exactly one credential"),
				   ReentrantInvalidationApplicator.ObservedCount, Fixture.Num());
	TestTrue(TEXT("Deferred invalidation completes once dispatch stack unwinds"), ReentrantFreshness.Invalidate());
	FUnrealAICredentialFreshnessSource BlockingFreshness;
	FUnrealAISecretValue BlockingSecret;
	TestTrue(TEXT("Hung-dispatch fixture secret constructs"), MakeSecret(Fixture, BlockingSecret, Error));
	FUnrealAICredentialLease BlockingLease;
	TestTrue(TEXT("Hung-dispatch fixture lease constructs"),
				  FUnrealAICredentialLease::TryCreate(Binding, Clock, BlockingFreshness.GetToken(), 300.0,
													  Now + FTimespan::FromMinutes(5), MoveTemp(BlockingSecret),
													  BlockingLease, Error));
	FBlockingCredentialApplicator BlockingApplicator(Binding);
	TFuture<bool> DispatchFuture = Async(EAsyncExecution::ThreadPool, [&BlockingLease, &BlockingApplicator, &Error]()
										 { return BlockingLease.TryApplyTo(BlockingApplicator, Error); });
	const bool bDispatchEntered = BlockingApplicator.WaitUntilEntered(2000);
	TestTrue(TEXT("Hung-dispatch fixture reaches transport admission"), bDispatchEntered);
	FEvent *ShutdownStarted = FPlatformProcess::GetSynchEventFromPool(false);
	TFuture<bool> ShutdownFuture = Async(EAsyncExecution::ThreadPool,
										 [&BlockingFreshness, ShutdownStarted]()
										 {
											 ShutdownStarted->Trigger();
											 return BlockingFreshness.BeginShutdown();
										 });
	TestTrue(TEXT("Shutdown attempt starts while dispatch is hung"), ShutdownStarted->Wait(2000));
	const bool bShutdownReturnedWhileHung = ShutdownFuture.WaitFor(FTimespan::FromSeconds(1.0));
	TestTrue(TEXT("Shutdown remains logically bounded while physical transport admission is hung"),
				  bShutdownReturnedWhileHung);
	if (bShutdownReturnedWhileHung)
	{
		TestFalse(TEXT("Contended shutdown reports deferred physical settlement"), ShutdownFuture.Get());
	}
	TestFalse(TEXT("Shutdown request immediately makes new freshness tokens fail closed"),
				   BlockingFreshness.GetToken().IsCurrent());
	BlockingApplicator.Release();
	TestTrue(TEXT("Hung dispatch fixture settles after physical release"),
				  DispatchFuture.WaitFor(FTimespan::FromSeconds(2.0)));
	if (!bShutdownReturnedWhileHung && ShutdownFuture.WaitFor(FTimespan::FromSeconds(2.0)))
	{
		ShutdownFuture.Get();
	}
	FPlatformProcess::ReturnSynchEventToPool(ShutdownStarted);
	TestTrue(TEXT("Deferred shutdown remains monotonic after dispatch releases"), BlockingFreshness.IsShutdown());
	TestFalse(TEXT("Invalidation cannot reopen a shutdown freshness source"), BlockingFreshness.Invalidate());

	bool bEveryShutdownInvalidateRaceStayedClosed = true;
	for (int32 Iteration = 0; Iteration < 64; ++Iteration)
	{
		FUnrealAICredentialFreshnessSource RacingFreshness;
		const FUnrealAICredentialFreshnessToken OldToken = RacingFreshness.GetToken();
		ParallelFor(2,
					[&RacingFreshness](const int32 Index)
					{
						if (Index == 0)
						{
							RacingFreshness.BeginShutdown();
						}
						else
						{
							RacingFreshness.Invalidate();
						}
					});
		bEveryShutdownInvalidateRaceStayedClosed &= RacingFreshness.IsShutdown() && !OldToken.IsCurrent() &&
													!RacingFreshness.GetToken().IsValid() &&
													!RacingFreshness.Invalidate();
	}
	TestTrue(TEXT("Concurrent shutdown and invalidation never reopen credential freshness"),
				  bEveryShutdownInvalidateRaceStayedClosed);

	auto ExpectBindingRejection =
		[this, &MakeBoundLease, &Error](const TCHAR *Label, const FUnrealAICredentialDestination &Destination)
	{
		FUnrealAICredentialLease MismatchLease;
		TestTrue(FString::Printf(TEXT("%s fixture lease constructs"), Label), MakeBoundLease(MismatchLease));
		FTestCredentialApplicator Applicator(Destination);
		TestFalse(Label, MismatchLease.TryApplyTo(Applicator, Error));
		TestEqual(FString::Printf(TEXT("%s does not expose bytes"), Label), Applicator.ObservedCount, 0);
		TestTrue(FString::Printf(TEXT("%s leaves lease available for its correct destination"), Label),
								 MismatchLease.IsValid());
	};

	FUnrealAICredentialDestination Mismatch = Binding;
	Mismatch.ModelProviderName = TEXT("openai.codex");
	ExpectBindingRejection(TEXT("Provider mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	Mismatch.AuthScheme = EUnrealAIAuthScheme::GatewayBearer;
	Mismatch.BillingMode = EUnrealAIBillingMode::GatewayAccounted;
	ExpectBindingRejection(TEXT("Auth scheme mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	Mismatch.AccountAuthProviderName = TEXT("openai.auth");
	ExpectBindingRejection(TEXT("Account auth provider mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	Mismatch.AuthProfileId = TEXT("tests.other_account");
	ExpectBindingRejection(TEXT("Account profile mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	Mismatch.AccountId.Value = FGuid(101, 102, 103, 104);
	ExpectBindingRejection(TEXT("Account identity mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	Mismatch.TenantRealm = TEXT("tests.tenant_b");
	ExpectBindingRejection(TEXT("Tenant mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	Mismatch.BillingPrincipalId.Value = FGuid(105, 106, 107, 108);
	ExpectBindingRejection(TEXT("Billing principal mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	Mismatch.PayerHandle = TEXT("tests.payer_b");
	ExpectBindingRejection(TEXT("Payer mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	Mismatch.Audience = TEXT("different-resource");
	ExpectBindingRejection(TEXT("Audience mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	TestTrue(TEXT("Alternate secure origin parses"),
				  FUnrealAIEndpointOrigin::TryParse(TEXT("https://api.x.ai"), false, Mismatch.EndpointOrigin, Error));
	ExpectBindingRejection(TEXT("Endpoint origin mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	++Mismatch.EndpointPolicyRevision;
	ExpectBindingRejection(TEXT("Endpoint policy revision mismatch is rejected"), Mismatch);
	Mismatch = Binding;
	++Mismatch.ConnectionRevision;
	ExpectBindingRejection(TEXT("Connection revision mismatch is rejected"), Mismatch);
	FUnrealAICredentialLease RevokedLease;
	TestTrue(TEXT("Revocation fixture lease constructs"), MakeBoundLease(RevokedLease));
	TestTrue(TEXT("Account freshness generation invalidates"), Freshness.Invalidate());
	FTestCredentialApplicator RevokedApplicator(Binding);
	TestFalse(TEXT("Account invalidation revokes an outstanding lease"),
				   RevokedLease.TryApplyTo(RevokedApplicator, Error));
	TestEqual(TEXT("Revoked lease never exposes bytes"), RevokedApplicator.ObservedCount, 0);
	TestFalse(TEXT("Revoked lease immediately wipes retained secret bytes"), RevokedLease.IsValid());
	TestTrue(TEXT("Revoked lease display becomes unset"), RevokedLease.GetRedactedDisplay().Contains(TEXT("unset")));
	const TSharedRef<FExpireAfterMonotonicReadClock, ESPMode::ThreadSafe> PermitDeadlineClock =
		MakeShared<FExpireAfterMonotonicReadClock, ESPMode::ThreadSafe>();
	FUnrealAISecretValue PermitDeadlineSecret;
	TestTrue(TEXT("Freshness-permit deadline secret constructs"), MakeSecret(Fixture, PermitDeadlineSecret, Error));
	FUnrealAICredentialLease PermitDeadlineLease;
	TestTrue(TEXT("Freshness-permit deadline lease constructs"),
				  FUnrealAICredentialLease::TryCreate(Binding, PermitDeadlineClock, Freshness.GetToken(), 300.0,
													  Now + FTimespan::FromMinutes(5), MoveTemp(PermitDeadlineSecret),
													  PermitDeadlineLease, Error));
	PermitDeadlineClock->Arm(1);
	FTestCredentialApplicator PermitDeadlineApplicator(Binding);
	TestFalse(TEXT("Deadline crossing before freshness permit exposure is rejected"),
				   PermitDeadlineLease.TryApplyTo(PermitDeadlineApplicator, Error));
	TestEqual(TEXT("Freshness-permit deadline race never exposes bytes"), PermitDeadlineApplicator.ObservedCount, 0);

	FUnrealAICredentialLease ExpiredLease;
	TestTrue(TEXT("Expiring fixture lease constructs"), MakeBoundLease(ExpiredLease));
	MutableClock->Advance(FTimespan::FromMinutes(5));
	FTestCredentialApplicator ExpiredApplicator(Binding);
	TestFalse(TEXT("Expired lease is rejected"), ExpiredLease.TryApplyTo(ExpiredApplicator, Error));
	TestEqual(TEXT("Expired lease never exposes bytes"), ExpiredApplicator.ObservedCount, 0);
	TestFalse(TEXT("Expired lease immediately wipes retained secret bytes"), ExpiredLease.IsValid());

	FUnrealAICredentialLease ReplacementTarget;
	FUnrealAISecretValue TargetSecret;
	TestTrue(TEXT("Replacement target secret constructs"), MakeSecret(Fixture, TargetSecret, Error));
	TestTrue(TEXT("Replacement target lease starts live"),
				  FUnrealAICredentialLease::TryCreate(Binding, Clock, Freshness.GetToken(), 30.0,
													  Now + FTimespan::FromHours(1), MoveTemp(TargetSecret),
													  ReplacementTarget, Error));
	FUnrealAISecretValue ReplacementSecret;
	TestTrue(TEXT("Replacement failure secret constructs"), MakeSecret(Fixture, ReplacementSecret, Error));
	FUnrealAICredentialDestination InvalidBinding = Binding;
	InvalidBinding.Audience.Reset();
	TestFalse(TEXT("Failed lease replacement rejects invalid binding"),
				   FUnrealAICredentialLease::TryCreate(InvalidBinding, Clock, Freshness.GetToken(), 30.0,
													   Now + FTimespan::FromHours(1), MoveTemp(ReplacementSecret),
													   ReplacementTarget, Error));
	TestFalse(TEXT("Failed lease replacement clears prior live lease"), ReplacementTarget.IsValid());
	TestFalse(TEXT("Failed lease replacement consumes and clears supplied secret"), ReplacementSecret.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIProtectedSecondaryCredentialLeaseTest,
								 "UnrealAI.Auth.ProtectedSecondaryCredentialIsClosedBoundAndOneShot",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIProtectedSecondaryCredentialLeaseTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAICredentialDestination Binding = BuildSubscriptionDestination(Error);
	TestTrue(TEXT("Protected-secondary subscription destination validates"), Binding.ValidateShape(Error));
	const FDateTime Now(2026, 7, 20, 12, 0, 0);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(Now, 100.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	FUnrealAICredentialFreshnessSource Freshness;

	auto MakeProtectedLease = [&Binding, &Clock, &Freshness, &Now, &Error](FUnrealAICredentialLease &OutLease)
	{
		FUnrealAISecretValue Bearer;
		FUnrealAISecretValue Routing;
		return MakeSecret({1, 2, 3}, Bearer, Error) && MakeSecret({4, 5}, Routing, Error) &&
			   FUnrealAICredentialLease::TryCreateWithProtectedSecondary(
				   Binding, Clock, Freshness.GetToken(), 60.0, Now + FTimespan::FromMinutes(5), MoveTemp(Bearer),
				   MoveTemp(Routing), OutLease, Error);
	};

	FUnrealAICredentialLease Lease;
	TestTrue(TEXT("Protected-secondary lease constructs"), MakeProtectedLease(Lease));
	FProtectedCredentialApplicator Exact(Binding);
	TestTrue(TEXT("Closed pair applicator receives both values in one admission"), Lease.TryApplyTo(Exact, Error));
	TestEqual(TEXT("Pair applicator runs exactly once"), Exact.PairApplyCount, 1);
	TestEqual(TEXT("Primary byte count is preserved"), Exact.PrimaryCount, 3);
	TestEqual(TEXT("Protected-secondary byte count is preserved"), Exact.SecondaryCount, 2);
	TestEqual(TEXT("Primary bytes are intact"), Exact.PrimarySum, uint32(6));
	TestEqual(TEXT("Protected-secondary bytes are intact"), Exact.SecondarySum, uint32(9));
	TestFalse(TEXT("Successful pair admission consumes the lease"), Lease.IsValid());
	TestFalse(TEXT("Consumed protected-secondary lease cannot replay"), Lease.TryApplyTo(Exact, Error));
	TestEqual(TEXT("Duplicate dispatch never reaches the pair applicator"), Exact.PairApplyCount, 1);

	FUnrealAICredentialLease LegacyRejectedLease;
	TestTrue(TEXT("Legacy-rejection fixture constructs"), MakeProtectedLease(LegacyRejectedLease));
	FTestCredentialApplicator LegacyApplicator(Binding);
	TestFalse(TEXT("Applicator without explicit pair support fails closed"),
				   LegacyRejectedLease.TryApplyTo(LegacyApplicator, Error));
	TestEqual(TEXT("Legacy applicator observes no primary bytes from a pair"), LegacyApplicator.ObservedCount, 0);
	TestFalse(TEXT("Unsupported pair presentation still consumes both secret owners"), LegacyRejectedLease.IsValid());

	FUnrealAICredentialLease MismatchLease;
	TestTrue(TEXT("Pair mismatch fixture constructs"), MakeProtectedLease(MismatchLease));
	FUnrealAICredentialDestination Mismatch = Binding;
	Mismatch.AccountId.Value = FGuid(9, 8, 7, 6);
	FProtectedCredentialApplicator MismatchApplicator(Mismatch);
	TestFalse(TEXT("Pair destination mismatch fails before materialization"),
				   MismatchLease.TryApplyTo(MismatchApplicator, Error));
	TestEqual(TEXT("Mismatch exposes neither credential"), MismatchApplicator.PairApplyCount, 0);
	TestTrue(TEXT("Mismatch leaves the lease usable only for its frozen destination"), MismatchLease.IsValid());
	FProtectedCredentialApplicator CorrectAfterMismatch(Binding);
	TestTrue(TEXT("Frozen destination may consume the untouched pair"),
				  MismatchLease.TryApplyTo(CorrectAfterMismatch, Error));

	FUnrealAISecretValue BearerWithoutRouting;
	FUnrealAISecretValue MissingRouting;
	TestTrue(TEXT("Missing-routing fixture bearer constructs"), MakeSecret({7}, BearerWithoutRouting, Error));
	FUnrealAICredentialLease InvalidLease;
	TestFalse(TEXT("Protected-secondary creation rejects a missing routing value"),
				   FUnrealAICredentialLease::TryCreateWithProtectedSecondary(
					   Binding, Clock, Freshness.GetToken(), 60.0, Now + FTimespan::FromMinutes(5),
					   MoveTemp(BearerWithoutRouting), MoveTemp(MissingRouting), InvalidLease, Error));
	TestFalse(TEXT("Rejected missing-routing creation wipes the bearer owner"), BearerWithoutRouting.IsSet());

	FUnrealAICredentialDestination ApiKeyBinding = BuildApiKeyDestination(Error);
	FUnrealAISecretValue ApiKey;
	FUnrealAISecretValue IllicitSecondary;
	TestTrue(TEXT("Wrong-scheme primary fixture constructs"), MakeSecret({8}, ApiKey, Error));
	TestTrue(TEXT("Wrong-scheme secondary fixture constructs"), MakeSecret({9}, IllicitSecondary, Error));
	TestFalse(TEXT("Protected-secondary creation rejects API-key billing"),
				   FUnrealAICredentialLease::TryCreateWithProtectedSecondary(
					   ApiKeyBinding, Clock, Freshness.GetToken(), 60.0, {}, MoveTemp(ApiKey),
					   MoveTemp(IllicitSecondary), InvalidLease, Error));
	TestFalse(TEXT("Wrong-scheme rejection wipes primary material"), ApiKey.IsSet());
	TestFalse(TEXT("Wrong-scheme rejection wipes secondary material"), IllicitSecondary.IsSet());

	FUnrealAICredentialLease RevokedLease;
	TestTrue(TEXT("Revocation fixture constructs"), MakeProtectedLease(RevokedLease));
	TestTrue(TEXT("Freshness invalidation succeeds before pair dispatch"), Freshness.Invalidate());
	FProtectedCredentialApplicator RevokedApplicator(Binding);
	TestFalse(TEXT("Revoked protected-secondary lease fails closed"),
				   RevokedLease.TryApplyTo(RevokedApplicator, Error));
	TestEqual(TEXT("Revocation exposes neither credential"), RevokedApplicator.PairApplyCount, 0);
	TestFalse(TEXT("Revoked pair is wiped"), RevokedLease.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIProviderAccessSupportClassificationTest,
								 "UnrealAI.Auth.SupportClassificationIsClosedAndExperimentalSubscriptionOnly",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIProviderAccessSupportClassificationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	FUnrealAIProviderAccessDescriptor SupportedApi;
	TestEqual(TEXT("Unclassified descriptors fail closed by default"), SupportedApi.SupportClassification,
				   EUnrealAIProviderAccessSupportClassification::Invalid);
	SupportedApi.ModelProviderName = TEXT("tests.supported_api");
	SupportedApi.AccountAuthProviderName = TEXT("tests.supported_api.auth");
	SupportedApi.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	SupportedApi.BillingMode = EUnrealAIBillingMode::ApiMetered;
	SupportedApi.Availability = EUnrealAIProviderAccessAvailability::ConfigurationRequired;
	TestFalse(TEXT("An otherwise valid descriptor cannot omit its support classification"),
				   SupportedApi.ValidateShape(Error));
	SupportedApi.SupportClassification = EUnrealAIProviderAccessSupportClassification::Supported;
	TestTrue(TEXT("An explicitly supported API-key descriptor remains valid"), SupportedApi.ValidateShape(Error));

	FUnrealAIProviderAccessDescriptor ExperimentalSubscription;
	ExperimentalSubscription.ModelProviderName = TEXT("tests.experimental_subscription");
	ExperimentalSubscription.AccountAuthProviderName = TEXT("tests.experimental_subscription.oauth");
	ExperimentalSubscription.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	ExperimentalSubscription.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	ExperimentalSubscription.Availability = EUnrealAIProviderAccessAvailability::SignedOut;
	ExperimentalSubscription.SupportClassification =
		EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
	TestTrue(TEXT("Experimental direct-subscription compatibility is programmatically visible and valid"),
				  ExperimentalSubscription.ValidateShape(Error));

	FUnrealAIProviderAccessDescriptor InvalidClassification = SupportedApi;
	InvalidClassification.SupportClassification = static_cast<EUnrealAIProviderAccessSupportClassification>(255);
	TestFalse(TEXT("Unknown support classifications fail closed"), InvalidClassification.ValidateShape(Error));

	FUnrealAIProviderAccessDescriptor CandidateApi = SupportedApi;
	CandidateApi.SupportClassification = EUnrealAIProviderAccessSupportClassification::ImplementationCandidate;
	TestTrue(TEXT("A public API-key implementation candidate is programmatically visible and valid"),
				  CandidateApi.ValidateShape(Error));

	FUnrealAIProviderAccessDescriptor CandidateSubscription = ExperimentalSubscription;
	CandidateSubscription.SupportClassification = EUnrealAIProviderAccessSupportClassification::ImplementationCandidate;
	TestFalse(TEXT("A subscription OAuth route cannot use the public API implementation-candidate classification"),
				   CandidateSubscription.ValidateShape(Error));

	FUnrealAIProviderAccessDescriptor ExperimentalApi = SupportedApi;
	ExperimentalApi.SupportClassification =
		EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
	TestFalse(TEXT("API-key billing cannot be labeled direct-subscription compatibility"),
				   ExperimentalApi.ValidateShape(Error));

	FUnrealAIProviderAccessDescriptor ContradictoryGated = ExperimentalSubscription;
	ContradictoryGated.Availability = EUnrealAIProviderAccessAvailability::PartnerGated;
	TestFalse(TEXT("Partner-gated and experimental compatibility classifications cannot be combined"),
				   ContradictoryGated.ValidateShape(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIConnectionPolicyTest, "UnrealAI.Auth.ConnectionSubscriptionAndPayerPolicy",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIConnectionPolicyTest::RunTest(const FString &Parameters)
{
	FString Error;
	FUnrealAIConnectionDescriptor ApiConnection = BuildApiKeyConnection(TEXT("tests.primary"), Error);
	TestTrue(TEXT("Custom API-key connection validates"), ApiConnection.ValidateShape(Error));

	FUnrealAIEndpointProfileRegistry EndpointProfiles;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> RegisteredEndpoint;
	TestTrue(TEXT("Custom endpoint enters only through typed API-key admission"),
				  EndpointProfiles.RegisterCustomApiEndpoint(
					  ApiConnection.EndpointProfileId, ApiConnection.CredentialDestination.ModelProviderName,
					  ApiConnection.CredentialDestination.EndpointOrigin, ApiConnection.CredentialDestination.Audience,
					  ApiConnection.CredentialDestination.EndpointPolicyRevision, RegisteredEndpoint, Error));
	const FUnrealAICredentialDestination OfflineDestination = BuildOfflineDestination();
	TestTrue(TEXT("Anonymous offline destination validates without a network origin"),
				  OfflineDestination.ValidateShape(Error));
	TestTrue(TEXT("Offline provider enters through typed local admission"),
				  EndpointProfiles.RegisterLocalInProcessEndpoint(
					  TEXT("tests.offline"), OfflineDestination.ModelProviderName,
						   OfflineDestination.EndpointPolicyRevision, RegisteredEndpoint, Error));
	FUnrealAIAccountAuthProviderRegistry AuthProviders;
	const TSharedRef<FTestAccountAuthProvider, ESPMode::ThreadSafe> AuthProvider =
		MakeShared<FTestAccountAuthProvider, ESPMode::ThreadSafe>();
	FUnrealAIProviderEndpointAuthority SubscriptionAuthority;
	TestTrue(TEXT("Unique account-auth provider registers and receives endpoint authority"),
				  AuthProviders.Register(AuthProvider, SubscriptionAuthority, Error));
	TestTrue(TEXT("Approved subscription authority freezes an exact model-provider resource"),
				  SubscriptionAuthority.Authorizes(TEXT("xai.grok"), EUnrealAIAuthScheme::OAuthBearer,
														EUnrealAIBillingMode::SubscriptionQuota));
	const TSharedRef<FTestAccountAuthProvider, ESPMode::ThreadSafe> DuplicateAuthProvider =
		MakeShared<FTestAccountAuthProvider, ESPMode::ThreadSafe>();
	FUnrealAIProviderEndpointAuthority RejectedAuthority;
	TestFalse(TEXT("Duplicate account-auth provider name is rejected"),
				   AuthProviders.Register(DuplicateAuthProvider, RejectedAuthority, Error));
	TestTrue(TEXT("Rejected provider receives no endpoint authority"),
				  RejectedAuthority.GetAuthProviderName().IsNone());
	const TSharedRef<FTestAccountAuthProvider, ESPMode::ThreadSafe> PartnerGatedProvider =
		MakeShared<FTestAccountAuthProvider, ESPMode::ThreadSafe>(
			TEXT("openai.auth"), EUnrealAIProviderAccessAvailability::PartnerGated);
	FUnrealAIProviderEndpointAuthority PartnerGatedAuthority;
	TestTrue(TEXT("Partner-gated provider remains discoverable without network authority"),
				  AuthProviders.Register(PartnerGatedProvider, PartnerGatedAuthority, Error));
	TestTrue(TEXT("Partner-gated provider receives no subscription endpoint authority"),
				  PartnerGatedAuthority.GetAuthProviderName().IsNone());
	const TSharedRef<const FUnrealAIAccountAuthProviderRegistrySnapshot, ESPMode::ThreadSafe> AuthSnapshot =
		AuthProviders.CreateSnapshot();
	TestEqual(TEXT("Auth-provider snapshot remains bounded and unique"), AuthSnapshot->Num(), 2);
	const TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> DiscoveredPartner =
		AuthSnapshot->Find(TEXT("openai.auth"));
	TestTrue(TEXT("Partner-gated provider is discoverable through the safe registry"), DiscoveredPartner.IsValid());
	TestEqual(TEXT("Discovery reports PartnerGated without OAuth registration details"),
				   DiscoveredPartner->DescribeAccess().Availability, EUnrealAIProviderAccessAvailability::PartnerGated);
	const FUnrealAIAccountAuthCapabilities GatedCapabilities = DiscoveredPartner->DescribeCapabilities();
	TestFalse(TEXT("Partner-gated provider advertises no browser authorization capability"),
				   GatedCapabilities.bBrowserPkce);
	TestFalse(TEXT("Partner-gated provider advertises no device authorization capability"),
				   GatedCapabilities.bDeviceCode);
	FUnrealAIInteractiveAuthRequest GatedRequest;
	GatedRequest.RequestId.Value = FGuid(401, 402, 403, 404);
	GatedRequest.AuthProfileId = TEXT("tests.partner_gated");
	GatedRequest.Flow = EUnrealAIInteractiveAuthFlow::BrowserPkce;
	GatedRequest.TimeoutSeconds = 30.0f;
	FUnrealAICancellationSource GatedCancellation;
	const TSharedRef<FDiscardingAuthEventSink, ESPMode::ThreadSafe> GatedSink =
		MakeShared<FDiscardingAuthEventSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> GatedHandle;
	FUnrealAIProviderAccessError GatedError;
	TestFalse(TEXT("Partner-gated sign-in fails deterministically before starting an operation"),
				   FUnrealAIProviderAccessTestGestureAuthority::StartSignIn(*DiscoveredPartner, GatedRequest, GatedSink,
																			GatedCancellation.GetToken(), GatedHandle,
																			GatedError));
	TestFalse(TEXT("Partner-gated sign-in returns no operation handle"), GatedHandle.IsValid());
	TestEqual(TEXT("Partner-gated sign-in returns its closed pre-network code"), GatedError.Code,
				   EUnrealAIProviderAccessErrorCode::PartnerGated);
	TestEqual(TEXT("Partner-gated sign-in emits no asynchronous event"), GatedSink->Count, 0);

	const TSharedRef<FTestAccountAuthProvider, ESPMode::ThreadSafe> MisadvertisingGatedProvider =
		MakeShared<FTestAccountAuthProvider, ESPMode::ThreadSafe>(
			TEXT("tests.bad_gated"), EUnrealAIProviderAccessAvailability::PartnerGated,
				 EUnrealAIAuthScheme::OAuthBearer, EUnrealAIBillingMode::SubscriptionQuota, true);
	FUnrealAIProviderEndpointAuthority MisadvertisingAuthority;
	TestFalse(TEXT("Registry rejects a gated provider that advertises interactive capabilities"),
				   AuthProviders.Register(MisadvertisingGatedProvider, MisadvertisingAuthority, Error));
	TestFalse(TEXT("Rejected gated capability advertisement receives no authority"), MisadvertisingAuthority.IsValid());

	const TSharedRef<FTestAccountAuthProvider, ESPMode::ThreadSafe> ApiKeyAuthProvider =
		MakeShared<FTestAccountAuthProvider, ESPMode::ThreadSafe>(
			TEXT("tests.api_auth"), EUnrealAIProviderAccessAvailability::Available, EUnrealAIAuthScheme::ApiKey,
				 EUnrealAIBillingMode::ApiMetered);
	FUnrealAIProviderEndpointAuthority ApiKeyAuthority;
	TestTrue(TEXT("Available API-key auth provider remains discoverable"),
				  AuthProviders.Register(ApiKeyAuthProvider, ApiKeyAuthority, Error));
	TestFalse(TEXT("API-key provider cannot receive subscription endpoint authority"), ApiKeyAuthority.IsValid());
	FUnrealAIConnectionDescriptor SubscriptionConnection;
	SubscriptionConnection.ConnectionAlias = TEXT("tests.subscription");
	SubscriptionConnection.EndpointProfileId = TEXT("xai.subscription_service");
	SubscriptionConnection.CredentialDestination = BuildSubscriptionDestination(Error);
	TestTrue(TEXT("Approved subscription resource enters through auth-provider authority"),
				  EndpointProfiles.RegisterProviderSubscriptionEndpoint(
					  SubscriptionAuthority, SubscriptionConnection.EndpointProfileId,
					  SubscriptionConnection.CredentialDestination.ModelProviderName,
					  SubscriptionConnection.CredentialDestination.EndpointOrigin,
					  SubscriptionConnection.CredentialDestination.Audience,
					  SubscriptionConnection.CredentialDestination.EndpointPolicyRevision, RegisteredEndpoint, Error));
	TestFalse(TEXT("Subscription authority cannot authorize another model provider"),
				   EndpointProfiles.RegisterProviderSubscriptionEndpoint(
					   SubscriptionAuthority,
					   TEXT("tests.cross_provider_subscription"),
							TEXT("openai.subscription"), SubscriptionConnection.CredentialDestination.EndpointOrigin,
								 SubscriptionConnection.CredentialDestination.Audience,
								 SubscriptionConnection.CredentialDestination.EndpointPolicyRevision,
								 RegisteredEndpoint, Error));
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const TCHAR *LoopbackText = Index == 0 ? TEXT("https://localhost")
											   : Index == 1
			? TEXT("https://127.0.0.1")
			: Index == 2
			? TEXT("https://127.0.0.2")
			: TEXT("https://[::1]");
		FUnrealAIEndpointOrigin LoopbackOrigin;
		TestTrue(FString::Printf(TEXT("Subscription loopback fixture %d parses"), Index),
								 FUnrealAIEndpointOrigin::TryParse(LoopbackText, false, LoopbackOrigin, Error));
		TestFalse(FString::Printf(TEXT("Subscription endpoint rejects secure loopback fixture %d"), Index),
								  EndpointProfiles.RegisterProviderSubscriptionEndpoint(
									  SubscriptionAuthority,
									  FName(*FString::Printf(TEXT("tests.subscription_loopback_%d"), Index)),
											SubscriptionConnection.CredentialDestination.ModelProviderName,
											LoopbackOrigin, SubscriptionConnection.CredentialDestination.Audience,
											SubscriptionConnection.CredentialDestination.EndpointPolicyRevision,
											RegisteredEndpoint, Error));
	}
	FUnrealAIConnectionRegistry AuthoritativeRegistry(EndpointProfiles.CreateSnapshot());
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> RegisteredConnection;
	TestTrue(TEXT("Provider-authorized subscription connection registers"),
				  AuthoritativeRegistry.Register(SubscriptionConnection, RegisteredConnection, Error));
	FUnrealAIConnectionDescriptor OfflineConnection;
	OfflineConnection.ConnectionAlias = TEXT("tests.offline");
	OfflineConnection.EndpointProfileId = TEXT("tests.offline");
	OfflineConnection.CredentialDestination = OfflineDestination;
	TestTrue(TEXT("Anonymous offline connection registers and needs no credential"),
				  AuthoritativeRegistry.Register(OfflineConnection, RegisteredConnection, Error));
	FUnrealAIConnectionDescriptor ForgedSubscription = SubscriptionConnection;
	ForgedSubscription.ConnectionAlias = TEXT("tests.forged_subscription");
	ForgedSubscription.EndpointProfileId = ApiConnection.EndpointProfileId;
	TestFalse(TEXT("Custom endpoint cannot self-assert subscription trust"),
				   AuthoritativeRegistry.Register(ForgedSubscription, RegisteredConnection, Error));
	ForgedSubscription = SubscriptionConnection;
	ForgedSubscription.ConnectionAlias = TEXT("tests.forged_auth_provider");
	ForgedSubscription.CredentialDestination.AccountAuthProviderName = TEXT("openai.auth");
	TestFalse(TEXT("Subscription endpoint authority cannot be reassigned to another auth provider"),
				   AuthoritativeRegistry.Register(ForgedSubscription, RegisteredConnection, Error));

	FUnrealAIConnectionDescriptor Candidate = ApiConnection;
	Candidate.ConnectionAlias = TEXT("tests.fallback");
	TestTrue(TEXT("Alias-only fallback with an identical frozen destination is allowed"),
				  FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate.ConnectionRevision = 2;
	Candidate.CredentialDestination.ConnectionRevision = 2;
	TestFalse(TEXT("Automatic fallback cannot change connection revision"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.EndpointProfileId = TEXT("tests.secondary_endpoint");
	TestFalse(TEXT("Automatic fallback cannot change endpoint profile"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.CredentialDestination.ModelProviderName = TEXT("other.compatible");
	TestFalse(TEXT("Automatic fallback cannot change model provider"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	TestTrue(TEXT("Fallback alternate origin parses"),
				  FUnrealAIEndpointOrigin::TryParse(TEXT("https://alternate.example.com"), false,
														 Candidate.CredentialDestination.EndpointOrigin, Error));
	TestFalse(TEXT("Automatic fallback cannot change endpoint origin"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.CredentialDestination.Audience = TEXT("alternate-audience");
	TestFalse(TEXT("Automatic fallback cannot change audience"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	++Candidate.CredentialDestination.EndpointPolicyRevision;
	TestFalse(TEXT("Automatic fallback cannot change endpoint policy revision"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.CredentialDestination.PayerHandle = TEXT("tests.payer_b");
	TestFalse(TEXT("Automatic fallback cannot change payer"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.ConnectionAlias = TEXT("tests.fallback_auth_scheme");
	Candidate.CredentialDestination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	TestFalse(TEXT("Automatic fallback cannot change credential scheme within one billing mode"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.ConnectionAlias = TEXT("tests.fallback_auth_provider");
	Candidate.CredentialDestination.AccountAuthProviderName = TEXT("other.secrets");
	TestFalse(TEXT("Automatic fallback cannot change account auth provider"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.CredentialDestination.AuthProfileId = TEXT("tests.other_profile");
	TestFalse(TEXT("Automatic fallback cannot change auth profile"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.CredentialDestination.AccountId.Value = FGuid(201, 202, 203, 204);
	TestFalse(TEXT("Automatic fallback cannot change local account identity"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.CredentialDestination.TenantRealm = TEXT("tests.other_tenant");
	TestFalse(TEXT("Automatic fallback cannot change tenant realm"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.CredentialDestination.BillingPrincipalId.Value = FGuid(205, 206, 207, 208);
	TestFalse(TEXT("Automatic fallback cannot change billing principal"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	Candidate = ApiConnection;
	Candidate.ConnectionAlias = TEXT("tests.fallback_subscription");
	Candidate.CredentialDestination = BuildSubscriptionDestination(Error);
	Candidate.EndpointProfileId = TEXT("xai.subscription_service");
	TestFalse(TEXT("Automatic fallback cannot cross subscription and API billing"),
				   FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(ApiConnection, Candidate, Error));
	TestFalse(TEXT("Registry cannot unregister a different instance with the same provider name"),
				   AuthProviders.Unregister(DuplicateAuthProvider, Error));
	TestEqual(TEXT("Rejected exact-instance removal preserves the live registry"), AuthProviders.Num(), 3);
	TestTrue(TEXT("Registry unregisters the exact provider instance"), AuthProviders.Unregister(AuthProvider, Error));
	TestEqual(TEXT("Exact-instance removal advances the live registry"), AuthProviders.Num(), 2);
	TestTrue(TEXT("Frozen auth-provider snapshots retain their admitted instance"),
				  AuthSnapshot->Find(AuthProvider->GetProviderName()).IsValid());
	TestFalse(TEXT("An already removed provider cannot be removed twice"),
				   AuthProviders.Unregister(AuthProvider, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIConnectionRegistryTest,
								 "UnrealAI.Auth.ConnectionRegistryCollisionSnapshotAndConcurrency",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIConnectionRegistryTest::RunTest(const FString &Parameters)
{
	FString Error;
	FUnrealAIConnectionRegistry Registry(BuildApiEndpointProfiles(Error));
	const FUnrealAIConnectionDescriptor First = BuildApiKeyConnection(TEXT("tests.connection_a"), Error);
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Registered;
	TestTrue(TEXT("First connection registers"), Registry.Register(First, Registered, Error));
	TestTrue(TEXT("Published descriptor remains available"), Registered.IsValid());
	TestEqual(TEXT("Generation advances for first registration"), Registry.GetGeneration(), uint64(1));
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Rejected;
	TestFalse(TEXT("Exact duplicate alias is rejected"), Registry.Register(First, Rejected, Error));
	TestTrue(TEXT("Duplicate diagnostic is stable"), Error.Contains(TEXT("duplicate")));

	FUnrealAIConnectionDescriptor Drift = First;
	Drift.CredentialDestination.PayerHandle = TEXT("tests.changed_payer");
	TestFalse(TEXT("Same-alias descriptor drift is rejected"), Registry.Register(Drift, Rejected, Error));
	TestTrue(TEXT("Drift diagnostic is stable"), Error.Contains(TEXT("drift")));

	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Frozen = Registry.CreateSnapshot();
	const FUnrealAIConnectionDescriptor Second = BuildApiKeyConnection(TEXT("tests.connection_b"), Error);
	TestTrue(TEXT("Second connection registers"), Registry.Register(Second, Registered, Error));
	TestEqual(TEXT("Frozen snapshot remains immutable"), Frozen->Num(), 1);
	TestEqual(TEXT("Live registry advances"), Registry.Num(), 2);
	TestEqual(TEXT("Snapshot generation remains frozen"), Frozen->GetGeneration(), uint64(1));

	constexpr int32 ConcurrentCount = 32;
	TAtomic<int32> SuccessCount{0};
	ParallelFor(ConcurrentCount,
				[&Registry, &SuccessCount](const int32 Index)
				{
					FString LocalError;
					const FUnrealAIConnectionDescriptor Descriptor =
						BuildApiKeyConnection(FString::Printf(TEXT("tests.concurrent_%d"), Index), LocalError);
					TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> LocalRegistered;
					if (Registry.Register(Descriptor, LocalRegistered, LocalError))
					{
						++SuccessCount;
					}
				});
	TestEqual(TEXT("Every unique concurrent connection registers"), SuccessCount.Load(), ConcurrentCount);
	TestEqual(TEXT("Registry contains deterministic complete set"), Registry.Num(), ConcurrentCount + 2);
	TestEqual(TEXT("Sorted snapshot enumerates every connection"), Registry.CreateSnapshot()->GetConnections().Num(),
				   ConcurrentCount + 2);
	int32 FillSuccessCount = 0;
	for (int32 Index = ConcurrentCount; Index < FUnrealAIConnectionRegistry::MaxConnections - 2; ++Index)
	{
		const FUnrealAIConnectionDescriptor Descriptor =
			BuildApiKeyConnection(FString::Printf(TEXT("tests.capacity_%d"), Index), Error);
		if (Registry.Register(Descriptor, Registered, Error))
		{
			++FillSuccessCount;
		}
	}
	TestEqual(TEXT("Registry accepts every entry through its exact capacity"), FillSuccessCount,
				   FUnrealAIConnectionRegistry::MaxConnections - ConcurrentCount - 2);
	TestEqual(TEXT("Registry reaches exact bounded capacity"), Registry.Num(),
				   FUnrealAIConnectionRegistry::MaxConnections);
	const FUnrealAIConnectionDescriptor Overflow = BuildApiKeyConnection(TEXT("tests.capacity_overflow"), Error);
	TestFalse(TEXT("Registry rejects capacity plus one"), Registry.Register(Overflow, Rejected, Error));
	TestTrue(TEXT("Capacity diagnostic is stable"), Error.Contains(TEXT("capacity")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIMemorySecretStoreTest,
								 "UnrealAI.Auth.MemorySecretStoreCasCancellationAndTimeout",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIMemorySecretStoreTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FDateTime Now(2026, 7, 20, 12, 0, 0);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(Now, 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	FUnrealAICancellationSource CancellationSource;
	FUnrealAISecretStoreOperationContext Context;
	TestTrue(TEXT("Bounded secret-store context constructs"),
				  FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, CancellationSource.GetToken(), Context,
																  Error));

	FUnrealAIMemorySecretStore Store(TEXT("tests.memory_store"), 2);
	TestFalse(TEXT("Memory store is explicitly non-persistent"), Store.DescribeCapabilities().bPersistent);
	TestTrue(TEXT("Memory store provides atomic compare-and-swap"), Store.DescribeCapabilities().bAtomicCompareAndSwap);
	FUnrealAISecretHandle Handle;
	Handle.StoreName = Store.GetStoreName();
	Handle.Value = FGuid(1, 2, 3, 4);
	FUnrealAISecretValue First;
	const TArray<uint8> FirstBytes{1, 2, 3};
	TestTrue(TEXT("First store secret constructs"), MakeSecret(FirstBytes, First, Error));
	uint64 Revision = 0;
	FUnrealAIProviderAccessError StoreError;
	TestEqual(TEXT("Create-only store succeeds"), Store.Store(Context, Handle, First, 0, Revision, StoreError),
				   EUnrealAISecretStoreResult::Succeeded);
	TestEqual(TEXT("First store revision is one"), Revision, uint64(1));
	uint64 RejectedRevision = 99;
	TestEqual(TEXT("Duplicate create-only write conflicts"),
				   Store.Store(Context, Handle, First, 0, RejectedRevision, StoreError),
				   EUnrealAISecretStoreResult::Conflict);
	TestEqual(TEXT("Rejected write clears output revision"), RejectedRevision, uint64(0));

	FUnrealAISecretValue Loaded;
	uint64 LoadedRevision = 0;
	TestEqual(TEXT("Stored secret loads"), Store.Load(Context, Handle, Loaded, LoadedRevision, StoreError),
				   EUnrealAISecretStoreResult::Succeeded);
	TestEqual(TEXT("Load returns current revision"), LoadedRevision, uint64(1));
	FUnrealAICredentialDestination Destination = BuildApiKeyDestination(Error);
	FUnrealAICredentialLease LoadedLease;
	FUnrealAICredentialFreshnessSource LoadedFreshness;
	TestTrue(TEXT("Loaded secret remains usable only through a bound lease"),
				  FUnrealAICredentialLease::TryCreate(Destination, Clock, LoadedFreshness.GetToken(), 30.0, {},
													  MoveTemp(Loaded), LoadedLease, Error));
	FTestCredentialApplicator Applicator(Destination);
	TestTrue(TEXT("Loaded credential applies to exact destination"), LoadedLease.TryApplyTo(Applicator, Error));
	TestEqual(TEXT("Loaded bytes round-trip exactly"), Applicator.ObservedSum, uint32(6));

	constexpr int32 CompetitorCount = 32;
	TAtomic<int32> CasWinners{0};
	ParallelFor(CompetitorCount,
				[&Store, &Context, &Handle, &CasWinners](const int32 Index)
				{
					FString LocalError;
					FUnrealAIProviderAccessError LocalStoreError;
					FUnrealAISecretValue Candidate;
					TArray<uint8> CandidateBytes{static_cast<uint8>(Index + 1)};
					FUnrealAISecretValue::TryCreate(MoveTemp(CandidateBytes), Candidate, LocalError);
					uint64 NewRevision = 0;
					if (Store.Store(Context, Handle, Candidate, 1, NewRevision, LocalStoreError) ==
						EUnrealAISecretStoreResult::Succeeded)
					{
						++CasWinners;
					}
				});
	TestEqual(TEXT("Exactly one concurrent refresh rotation wins CAS"), CasWinners.Load(), 1);
	TestEqual(TEXT("Concurrent refresh publishes one replacement revision"),
				   Store.Load(Context, Handle, Loaded, LoadedRevision, StoreError),
				   EUnrealAISecretStoreResult::Succeeded);
	TestEqual(TEXT("CAS winner advances revision exactly once"), LoadedRevision, uint64(2));

	FUnrealAICancellationSource CancelledSource;
	FUnrealAISecretStoreOperationContext CancelledContext;
	TestTrue(TEXT("Cancellation context constructs"),
				  FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, CancelledSource.GetToken(),
																  CancelledContext, Error));
	CancelledSource.Cancel(EUnrealAICancellationReason::Requested);
	LoadedRevision = 99;
	TestEqual(TEXT("Cancelled load fails before exposure"),
				   Store.Load(CancelledContext, Handle, Loaded, LoadedRevision, StoreError),
				   EUnrealAISecretStoreResult::Cancelled);
	TestFalse(TEXT("Cancelled load clears secret output"), Loaded.IsSet());
	TestEqual(TEXT("Cancelled load clears revision output"), LoadedRevision, uint64(0));

	FUnrealAICancellationSource TimedSource;
	FUnrealAISecretStoreOperationContext TimedContext;
	TestTrue(TEXT("Timeout context constructs"), FUnrealAISecretStoreOperationContext::TryCreate(
													 Clock, 1.0, TimedSource.GetToken(), TimedContext, Error));
	MutableClock->Advance(FTimespan::FromSeconds(1));
	TestEqual(TEXT("Expired context fails before store access"),
				   Store.Load(TimedContext, Handle, Loaded, LoadedRevision, StoreError),
				   EUnrealAISecretStoreResult::TimedOut);

	FUnrealAICancellationSource StoreRaceSource;
	const TSharedRef<FCancelOnMonotonicReadClock, ESPMode::ThreadSafe> StoreRaceClock =
		MakeShared<FCancelOnMonotonicReadClock, ESPMode::ThreadSafe>(StoreRaceSource);
	FUnrealAISecretStoreOperationContext StoreRaceContext;
	TestTrue(TEXT("Store commit-race context constructs"),
				  FUnrealAISecretStoreOperationContext::TryCreate(StoreRaceClock, 30.0, StoreRaceSource.GetToken(),
																  StoreRaceContext, Error));
	StoreRaceClock->Arm(2);
	FUnrealAISecretHandle RaceHandle;
	RaceHandle.StoreName = Store.GetStoreName();
	RaceHandle.Value = FGuid(101, 102, 103, 104);
	FUnrealAISecretValue RaceValue;
	TestTrue(TEXT("Store commit-race secret constructs"), MakeSecret(FirstBytes, RaceValue, Error));
	uint64 RaceRevision = 99;
	TestEqual(TEXT("Cancellation between copy and commit rejects the write"),
				   Store.Store(StoreRaceContext, RaceHandle, RaceValue, 0, RaceRevision, StoreError),
				   EUnrealAISecretStoreResult::Cancelled);
	TestEqual(TEXT("Cancelled commit clears the output revision"), RaceRevision, uint64(0));
	TestEqual(TEXT("Cancelled commit does not create a record"), Store.Num(), 1);
	FUnrealAICancellationSource StoreDeadlineSource;
	const TSharedRef<FExpireAfterMonotonicReadClock, ESPMode::ThreadSafe> StoreDeadlineClock =
		MakeShared<FExpireAfterMonotonicReadClock, ESPMode::ThreadSafe>();
	FUnrealAISecretStoreOperationContext StoreDeadlineContext;
	TestTrue(TEXT("Store deadline-race context constructs"),
				  FUnrealAISecretStoreOperationContext::TryCreate(
					  StoreDeadlineClock, 30.0, StoreDeadlineSource.GetToken(), StoreDeadlineContext, Error));
	StoreDeadlineClock->Arm(2);
	RaceRevision = 99;
	TestEqual(TEXT("Deadline crossing between copy and commit rejects the write"),
				   Store.Store(StoreDeadlineContext, RaceHandle, RaceValue, 0, RaceRevision, StoreError),
				   EUnrealAISecretStoreResult::TimedOut);
	TestEqual(TEXT("Timed-out commit clears the output revision"), RaceRevision, uint64(0));
	TestEqual(TEXT("Timed-out commit does not create a record"), Store.Num(), 1);

	FUnrealAICancellationSource DeleteSource;
	FUnrealAISecretStoreOperationContext DeleteContext;
	TestTrue(TEXT("Delete context constructs"), FUnrealAISecretStoreOperationContext::TryCreate(
													Clock, 30.0, DeleteSource.GetToken(), DeleteContext, Error));
	TestEqual(TEXT("Stale delete revision conflicts"), Store.Delete(DeleteContext, Handle, 1, StoreError),
				   EUnrealAISecretStoreResult::Conflict);
	FUnrealAICancellationSource DeleteRaceSource;
	const TSharedRef<FCancelOnMonotonicReadClock, ESPMode::ThreadSafe> DeleteRaceClock =
		MakeShared<FCancelOnMonotonicReadClock, ESPMode::ThreadSafe>(DeleteRaceSource);
	FUnrealAISecretStoreOperationContext DeleteRaceContext;
	TestTrue(TEXT("Delete commit-race context constructs"),
				  FUnrealAISecretStoreOperationContext::TryCreate(DeleteRaceClock, 30.0, DeleteRaceSource.GetToken(),
																  DeleteRaceContext, Error));
	DeleteRaceClock->Arm(1);
	TestEqual(TEXT("Cancellation between delete admission and commit preserves the record"),
				   Store.Delete(DeleteRaceContext, Handle, 2, StoreError), EUnrealAISecretStoreResult::Cancelled);
	TestEqual(TEXT("Cancelled delete leaves the record present"), Store.Num(), 1);
	FUnrealAICancellationSource DeleteDeadlineSource;
	const TSharedRef<FExpireAfterMonotonicReadClock, ESPMode::ThreadSafe> DeleteDeadlineClock =
		MakeShared<FExpireAfterMonotonicReadClock, ESPMode::ThreadSafe>();
	FUnrealAISecretStoreOperationContext DeleteDeadlineContext;
	TestTrue(TEXT("Delete deadline-race context constructs"),
				  FUnrealAISecretStoreOperationContext::TryCreate(
					  DeleteDeadlineClock, 30.0, DeleteDeadlineSource.GetToken(), DeleteDeadlineContext, Error));
	DeleteDeadlineClock->Arm(1);
	TestEqual(TEXT("Deadline crossing between delete admission and commit preserves the record"),
				   Store.Delete(DeleteDeadlineContext, Handle, 2, StoreError), EUnrealAISecretStoreResult::TimedOut);
	TestEqual(TEXT("Timed-out delete leaves the record present"), Store.Num(), 1);
	TestEqual(TEXT("Exact revision deletes and wipes record"), Store.Delete(DeleteContext, Handle, 2, StoreError),
				   EUnrealAISecretStoreResult::Succeeded);
	TestEqual(TEXT("Deleted store is empty"), Store.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIAuthInteractionTest, "UnrealAI.Auth.AuthInteractionOriginAndRedaction",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIAuthInteractionTest::RunTest(const FString &Parameters)
{
	FString Error;
	FUnrealAIEndpointOrigin ApprovedOrigin;
	TestTrue(TEXT("Approved interaction origin parses"),
				  FUnrealAIEndpointOrigin::TryParse(TEXT("https://login.xai.example"), false, ApprovedOrigin, Error));
	FUnrealAIAuthInteraction Interaction;
	TestTrue(TEXT("Provider-bound device interaction constructs"),
				  FUnrealAIAuthInteraction::TryCreateDeviceCode(
					  ApprovedOrigin, FString(TEXT("https://login.xai.example/device")), FString(TEXT("ABCD-EFGH")),
																								 Interaction, Error));
	TestTrue(TEXT("Constructed interaction validates"), Interaction.IsValid());
	TestFalse(TEXT("Redacted interaction display excludes user code"),
				   Interaction.GetRedactedDisplay().Contains(TEXT("ABCD")));

	FUnrealAIAuthEvent Event;
	Event.RequestId.Value = FGuid(21, 22, 23, 24);
	Event.AuthProfileId = TEXT("tests.account");
	Event.OperationKind = EUnrealAIAuthOperationKind::SignIn;
	Event.Kind = EUnrealAIAuthEventKind::InteractionRequired;
	Event.State = EUnrealAIAccountAuthState::Authorizing;
	Event.Interaction = MakeUnique<FUnrealAIAuthInteraction>(MoveTemp(Interaction));
	TestTrue(TEXT("Interaction event validates for trusted local UI"), Event.ValidateShape(Error));
	Event.State = EUnrealAIAccountAuthState::Ready;
	TestFalse(TEXT("Interaction event rejects contradictory Ready state"), Event.ValidateShape(Error));

	FUnrealAIAuthInteraction Rejected;
	TestFalse(TEXT("Lookalike interaction origin is rejected"),
				   FUnrealAIAuthInteraction::TryCreateDeviceCode(ApprovedOrigin,
																 FString(TEXT("https://login.xai.example.evil/device")),
																		 FString(TEXT("ABCD-EFGH")), Rejected, Error));
	TestFalse(TEXT("Verification query is rejected to keep user code out of URLs"),
				   FUnrealAIAuthInteraction::TryCreateDeviceCode(
					   ApprovedOrigin, FString(TEXT("https://login.xai.example/device?user_code=ABCD")),
											   FString(TEXT("ABCD")), Rejected, Error));
	TestFalse(TEXT("Verification URI rejects embedded control characters"),
				   FUnrealAIAuthInteraction::TryCreateDeviceCode(ApprovedOrigin,
																 FString(TEXT("https://login.xai.example/devi\nce")),
																		 FString(TEXT("ABCD")), Rejected, Error));
	FUnrealAIAuthInteraction BrowserInteraction;
	TestTrue(
		TEXT("Origin-bound PKCE S256 browser interaction accepts required query material"),
			 FUnrealAIAuthInteraction::TryCreateBrowserLaunch(
				 ApprovedOrigin,
				 FString(TEXT("https://login.xai.example/authorize?client_id=test&state=opaque-state&code_challenge="
								   "opaque-challenge&code_challenge_method=S256")),
						 BrowserInteraction, Error));
	TestEqual(TEXT("Browser interaction retains typed launch shape"), BrowserInteraction.GetKind(),
				   EUnrealAIAuthInteractionKind::BrowserLaunch);
	TestTrue(TEXT("Browser interaction remains redacted"),
				  BrowserInteraction.GetRedactedDisplay().Contains(TEXT("redacted")));
	TestFalse(
		TEXT("Browser interaction rejects a missing state parameter"),
			 FUnrealAIAuthInteraction::TryCreateBrowserLaunch(
				 ApprovedOrigin,
				 FString(TEXT("https://login.xai.example/authorize?code_challenge=opaque&code_challenge_method=S256")),
						 Rejected, Error));
	TestFalse(
		TEXT("Browser interaction rejects duplicate state parameters"),
			 FUnrealAIAuthInteraction::TryCreateBrowserLaunch(
				 ApprovedOrigin,
				 FString(TEXT("https://login.xai.example/authorize?state=first&state=second&code_challenge=opaque&"
						 "code_challenge_method=S256")),
						 Rejected, Error));
	TestFalse(
		TEXT("Browser interaction rejects a PKCE downgrade hidden after S256"),
			 FUnrealAIAuthInteraction::TryCreateBrowserLaunch(
				 ApprovedOrigin, FString(TEXT("https://login.xai.example/authorize?state=opaque&code_challenge=opaque&"
						 "code_challenge_method=S256&code_challenge_method=plain")),
										 Rejected, Error));
	TestFalse(
		TEXT("Browser interaction rejects percent-encoded duplicate critical parameter names"),
			 FUnrealAIAuthInteraction::TryCreateBrowserLaunch(
				 ApprovedOrigin,
				 FString(TEXT("https://login.xai.example/authorize?state=opaque&%73tate=second&code_challenge=opaque&"
						 "code_challenge_method=S256")),
						 Rejected, Error));
	TestTrue(TEXT("Browser interaction accepts well-formed percent encoding in parameter values"),
		FUnrealAIAuthInteraction::TryCreateBrowserLaunch(
			ApprovedOrigin,
			FString(TEXT("https://login.xai.example/authorize?state=opaque%2Dstate&code_challenge=opaque%2Dchallenge&"
						 "code_challenge_method=S256")),
					BrowserInteraction, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAICredentialBrokerTerminalRaceTest,
								 "UnrealAI.Auth.CredentialBrokerCancelTimeoutShutdownTerminalRace",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAICredentialBrokerTerminalRaceTest::RunTest(const FString &Parameters)
{
	FUnrealAICredentialRequest Request;
	Request.RequestId.Value = FGuid(31, 32, 33, 34);
	Request.ConnectionAlias = TEXT("tests.connection");
	Request.TimeoutSeconds = 30.0f;
	FUnrealAICancellationSource CancellationSource;
	const TSharedRef<FTestCredentialResultSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FTestCredentialResultSink, ESPMode::ThreadSafe>();
	FTestCredentialBroker Broker;
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> Handle;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("Valid broker request is admitted with a handle"),
				  Broker.StartResolve(Request, Sink, CancellationSource.GetToken(), Handle, Error));
	TestTrue(TEXT("Accepted broker request returns exact request handle"), Handle.IsValid());
	TestEqual(TEXT("Broker handle preserves request identity"), Handle->GetRequestId(), Request.RequestId);
	const TSharedPtr<FTestCredentialOperationState, ESPMode::ThreadSafe> State = Broker.GetLastState();
	TestTrue(TEXT("Accepted broker request owns terminal state"), State.IsValid());

	ParallelFor(64,
				[&State, &Handle](const int32 Index)
				{
					if (Index % 3 == 0)
					{
						Handle->Cancel();
					}
					else if (Index % 3 == 1)
					{
						State->TrySettle(EUnrealAICredentialResultKind::TimedOut);
					}
					else
					{
						State->TrySettle(EUnrealAICredentialResultKind::Failed);
					}
				});
	TestEqual(TEXT("Cancel, timeout, failure, and shutdown-style races publish exactly one terminal"), Sink->GetCount(),
				   1);
	TestTrue(TEXT("Published race winner is a legal terminal"),
				  Sink->GetLastKind() == EUnrealAICredentialResultKind::Cancelled ||
					  Sink->GetLastKind() == EUnrealAICredentialResultKind::TimedOut ||
					  Sink->GetLastKind() == EUnrealAICredentialResultKind::Failed);

	FUnrealAICancellationSource PreCancelled;
	PreCancelled.Cancel(EUnrealAICancellationReason::Shutdown);
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RejectedHandle;
	TestFalse(TEXT("Pre-cancelled broker request fails synchronously"),
				   Broker.StartResolve(Request, Sink, PreCancelled.GetToken(), RejectedHandle, Error));
	TestFalse(TEXT("Synchronous rejection returns no handle"), RejectedHandle.IsValid());
	TestEqual(TEXT("Synchronous rejection does not publish an async terminal"), Sink->GetCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIProviderAccessResultShapeTest, "UnrealAI.Auth.AuthAndCredentialResultShape",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIProviderAccessResultShapeTest::RunTest(const FString &Parameters)
{
	FString Error;
	FUnrealAIInteractiveAuthRequest Request;
	Request.RequestId.Value = FGuid(1, 2, 3, 4);
	Request.AuthProfileId = TEXT("tests.account");
	Request.Flow = EUnrealAIInteractiveAuthFlow::DeviceCode;
	Request.TimeoutSeconds = 300.0f;
	TestTrue(TEXT("Bounded interactive auth request validates"), Request.ValidateShape(Error));
	Request.TimeoutSeconds = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("Non-finite auth timeout is rejected"), Request.ValidateShape(Error));
	Request.TimeoutSeconds = FUnrealAIInteractiveAuthRequest::MaxTimeoutSeconds;
	TestTrue(TEXT("Interactive auth exact timeout bound validates"), Request.ValidateShape(Error));
	Request.TimeoutSeconds = FUnrealAIInteractiveAuthRequest::MaxTimeoutSeconds + 1.0f;
	TestFalse(TEXT("Interactive auth timeout bound plus one is rejected"), Request.ValidateShape(Error));
	FUnrealAIAccountAuthRequest SignOutRequest;
	SignOutRequest.RequestId = Request.RequestId;
	SignOutRequest.AuthProfileId = Request.AuthProfileId;
	SignOutRequest.TimeoutSeconds = 30.0f;
	TestFalse(TEXT("Sign-out request cannot target an implicit or most-recent account"),
				   SignOutRequest.ValidateShape(Error));
	SignOutRequest.AccountId.Value = FGuid(13, 14, 15, 16);
	TestTrue(TEXT("Sign-out request targets one exact opaque account without an interactive flow"),
				  SignOutRequest.ValidateShape(Error));

	FUnrealAIAccountStatus AccountStatus;
	AccountStatus.ProviderName = TEXT("xai.grok");
	AccountStatus.AuthProfileId = TEXT("tests.account");
	AccountStatus.State = EUnrealAIAccountAuthState::Ready;
	TestFalse(TEXT("Ready account status requires an opaque local account handle"), AccountStatus.ValidateShape(Error));
	AccountStatus.AccountId.Value = FGuid(9, 10, 11, 12);
	TestTrue(TEXT("Ready account status with opaque local handle validates"), AccountStatus.ValidateShape(Error));
	AccountStatus.State = EUnrealAIAccountAuthState::SignedOut;
	TestFalse(TEXT("Signed-out status cannot retain account metadata"), AccountStatus.ValidateShape(Error));

	FUnrealAIAuthEvent Event;
	Event.RequestId.Value = FGuid(1, 2, 3, 4);
	Event.AuthProfileId = TEXT("tests.account");
	Event.OperationKind = EUnrealAIAuthOperationKind::SignIn;
	Event.Kind = EUnrealAIAuthEventKind::Succeeded;
	Event.State = EUnrealAIAccountAuthState::Ready;
	TestFalse(TEXT("Successful sign-in cannot omit the newly selected opaque account"), Event.ValidateShape(Error));
	Event.AccountId.Value = FGuid(21, 22, 23, 24);
	TestTrue(TEXT("Safe successful auth event identifies the selected account"), Event.ValidateShape(Error));
	Event.AccountId.Value = FGuid(25, 26, 27, 28);
	TestTrue(TEXT("A second signed-in account has a distinct valid terminal identity"), Event.ValidateShape(Error));
	Event.OperationKind = EUnrealAIAuthOperationKind::SignOut;
	Event.State = EUnrealAIAccountAuthState::SignedOut;
	TestTrue(TEXT("Successful sign-out terminal identifies one exact account as SignedOut"),
				  Event.ValidateShape(Error));
	Event.State = EUnrealAIAccountAuthState::Ready;
	TestFalse(TEXT("Successful sign-out cannot terminate Ready"), Event.ValidateShape(Error));
	Event.OperationKind = EUnrealAIAuthOperationKind::SignIn;
	Event.State = EUnrealAIAccountAuthState::SignedOut;
	TestFalse(TEXT("Successful sign-in cannot terminate SignedOut"), Event.ValidateShape(Error));
	Event.OperationKind = EUnrealAIAuthOperationKind::Invalid;
	TestFalse(TEXT("Auth event rejects an unknown operation kind"), Event.ValidateShape(Error));
	Event.OperationKind = EUnrealAIAuthOperationKind::SignIn;
	Event.Kind = EUnrealAIAuthEventKind::Cancelled;
	Event.State = EUnrealAIAccountAuthState::SignedOut;
	Event.Error = MakeSafeError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
	TestTrue(TEXT("Safe cancelled auth event validates"), Event.ValidateShape(Error));
	Event.Error = FUnrealAIProviderAccessError{};
	TestFalse(TEXT("Cancelled auth event requires structured error"), Event.ValidateShape(Error));
	Event.Error = MakeSafeError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::AuthTimedOut);
	TestFalse(TEXT("Cancelled auth event rejects a timeout category"), Event.ValidateShape(Error));
	Event.Error = MakeSafeError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
	Event.Error.Code = static_cast<EUnrealAIProviderAccessErrorCode>(255);
	TestFalse(TEXT("Auth events reject unknown provider-access error codes"), Event.ValidateShape(Error));
	Event.Kind = EUnrealAIAuthEventKind::Failed;
	Event.Error = MakeSafeError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
	TestFalse(TEXT("Failed auth terminal rejects cancellation category and code"), Event.ValidateShape(Error));
	const TPair<EUnrealAIProviderAccessErrorCode, EUnrealAIErrorCategory> AuthValidationMappings[] = {
		{EUnrealAIProviderAccessErrorCode::AuthResponseInvalid, EUnrealAIErrorCategory::Provider},
		{EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete, EUnrealAIErrorCategory::Provider},
		{EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient, EUnrealAIErrorCategory::NotAuthorized},
		{EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported, EUnrealAIErrorCategory::Provider},
		{EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid, EUnrealAIErrorCategory::Provider},
		{EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, EUnrealAIErrorCategory::Persistence}};
	for (const TPair<EUnrealAIProviderAccessErrorCode, EUnrealAIErrorCategory> &Mapping : AuthValidationMappings)
	{
		Event.Error = MakeSafeError(Mapping.Value, Mapping.Key);
		TestTrue(TEXT("Every safe auth-validation failure is admitted by a failed auth terminal"),
					  Event.ValidateShape(Error));
		Event.Error.Category = EUnrealAIErrorCategory::Internal;
		TestFalse(TEXT("Every safe auth-validation code rejects a non-canonical category"), Event.ValidateShape(Error));
	}
	FUnrealAIProviderAccessError BoundedError =
		MakeSafeError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
	BoundedError.bRetryable = true;
	BoundedError.RetryAfterSeconds = FUnrealAIProviderAccessError::MaxRetryAfterSeconds;
	TestTrue(TEXT("Provider-access retry delay accepts its exact bound"), BoundedError.ValidateShape(Error));
	BoundedError.RetryAfterSeconds = FUnrealAIProviderAccessError::MaxRetryAfterSeconds + 1.0f;
	TestFalse(TEXT("Provider-access retry delay rejects bound plus one"), BoundedError.ValidateShape(Error));
	BoundedError = MakeSafeError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::SecretNotFound);
	TestFalse(TEXT("Provider-access error rejects mismatched closed code and category"),
				   BoundedError.ValidateShape(Error));
	BoundedError = FUnrealAIProviderAccessError{};
	BoundedError.bRetryable = true;
	TestFalse(TEXT("Empty provider-access error rejects retry policy"), BoundedError.ValidateShape(Error));
	FUnrealAIProviderAccessError OperationBusy =
		MakeSafeError(EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::OperationBusy);
	TestTrue(TEXT("A provider operation in progress has a canonical busy error"), OperationBusy.ValidateShape(Error));
	OperationBusy.Category = EUnrealAIErrorCategory::Provider;
	TestFalse(TEXT("Operation-busy rejects a non-canonical category"), OperationBusy.ValidateShape(Error));
	const TPair<EUnrealAIProviderAccessErrorCode, EUnrealAIErrorCategory> StoreFaultMappings[] = {
		{EUnrealAIProviderAccessErrorCode::SecretStoreLocked, EUnrealAIErrorCategory::Busy},
		{EUnrealAIProviderAccessErrorCode::SecretStoreDenied, EUnrealAIErrorCategory::PolicyDenied},
		{EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, EUnrealAIErrorCategory::Persistence},
		{EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported, EUnrealAIErrorCategory::UnsupportedCapability},
		{EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt, EUnrealAIErrorCategory::Persistence}};
	for (const TPair<EUnrealAIProviderAccessErrorCode, EUnrealAIErrorCategory> &Mapping : StoreFaultMappings)
	{
		FUnrealAIProviderAccessError StoreFault = MakeSafeError(Mapping.Value, Mapping.Key);
		TestTrue(TEXT("Every advertised secure-store fault has a closed canonical error mapping"),
					  StoreFault.ValidateShape(Error));
		StoreFault.Category = EUnrealAIErrorCategory::Internal;
		TestFalse(TEXT("Secure-store fault code rejects a non-canonical category"), StoreFault.ValidateShape(Error));
	}

	FUnrealAICredentialRequest CredentialRequest;
	CredentialRequest.RequestId.Value = FGuid(5, 6, 7, 8);
	CredentialRequest.ConnectionAlias = TEXT("tests.connection");
	CredentialRequest.TimeoutSeconds = FUnrealAICredentialRequest::MaxTimeoutSeconds;
	TestTrue(TEXT("Credential request resolves only a trusted alias at exact timeout bound"),
				  CredentialRequest.ValidateShape(Error));
	CredentialRequest.TimeoutSeconds += 1.0f;
	TestFalse(TEXT("Credential request timeout bound plus one is rejected"), CredentialRequest.ValidateShape(Error));

	FUnrealAICredentialResult Result;
	Result.RequestId.Value = FGuid(9, 10, 11, 12);
	Result.Kind = EUnrealAICredentialResultKind::Failed;
	Result.Error =
		MakeSafeError(EUnrealAIErrorCategory::NotAuthorized, EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	TestTrue(TEXT("Failed credential result is terminal and structured"), Result.ValidateShape(Error));
	Result.Error =
		MakeSafeError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::CredentialCancelled);
	TestFalse(TEXT("Failed credential terminal rejects cancellation category and code"), Result.ValidateShape(Error));
	Result.Kind = EUnrealAICredentialResultKind::Succeeded;
	Result.Error = FUnrealAIProviderAccessError{};
	TestFalse(TEXT("Successful credential result cannot omit lease"), Result.ValidateShape(Error));
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> ResultClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 20), 10.0);
	FUnrealAICredentialFreshnessSource ResultFreshness;
	FUnrealAISecretValue ResultSecret;
	TestTrue(TEXT("Expired-result fixture secret constructs"), MakeSecret(TArray<uint8>{1, 2, 3}, ResultSecret, Error));
	FUnrealAICredentialLease ExpiredResultLease;
	TestTrue(TEXT("Expired-result fixture lease constructs"),
				  FUnrealAICredentialLease::TryCreate(BuildApiKeyDestination(Error), ResultClock,
													  ResultFreshness.GetToken(), 1.0, {}, MoveTemp(ResultSecret),
													  ExpiredResultLease, Error));
	Result.AccessContext = IUnrealAIProviderAccessContext::CreateCredentialed(MoveTemp(ExpiredResultLease));
	ResultClock->Advance(FTimespan::FromSeconds(1.0));
	TestFalse(TEXT("Successful credential result rejects an already-expired lease"), Result.ValidateShape(Error));
	Result.AccessContext.Reset();
	Result.Kind = EUnrealAICredentialResultKind::NotRequired;
	Result.Error = FUnrealAIProviderAccessError{};
	FUnrealAICredentialFreshnessSource ResultAnonymousFreshness;
	Result.AccessContext =
		IUnrealAIProviderAccessContext::CreateAnonymous(BuildOfflineDestination(), ResultAnonymousFreshness.GetToken());
	TestTrue(TEXT("Anonymous/local route can terminate as credential-not-required"), Result.ValidateShape(Error));
	const FUnrealAICredentialDestination AnonymousBinding = BuildOfflineDestination();
	FUnrealAICredentialFreshnessSource AnonymousFreshness;
	TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AnonymousContext =
		IUnrealAIProviderAccessContext::CreateAnonymous(AnonymousBinding, AnonymousFreshness.GetToken());
	FUnrealAICredentialDestination RetargetedAnonymous = AnonymousBinding;
	RetargetedAnonymous.TenantRealm = TEXT("tests.other_local_tenant");
	FTestCredentialApplicator RetargetedAnonymousDispatcher(RetargetedAnonymous);
	TestFalse(TEXT("Opaque anonymous context rejects tenant retargeting"),
				   AnonymousContext->TryDispatch(RetargetedAnonymousDispatcher, Error));
	FTestCredentialApplicator RetargetConsumedDispatcher(AnonymousBinding);
	TestFalse(TEXT("Failed anonymous dispatch still consumes the one-shot context"),
				   AnonymousContext->TryDispatch(RetargetConsumedDispatcher, Error));
	const TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> ExactAnonymousContext =
		IUnrealAIProviderAccessContext::CreateAnonymous(AnonymousBinding, AnonymousFreshness.GetToken());
	FTestCredentialApplicator AnonymousDispatcher(AnonymousBinding);
	TestTrue(TEXT("Opaque anonymous access context dispatches without credential bytes"),
				  ExactAnonymousContext->TryDispatch(AnonymousDispatcher, Error));
	TestTrue(TEXT("Anonymous dispatcher receives a credential-free admission"),
				  AnonymousDispatcher.bAnonymousDispatched);
	FTestCredentialApplicator DuplicateAnonymousDispatcher(AnonymousBinding);
	TestFalse(TEXT("Opaque anonymous access context rejects a second dispatch"),
				   ExactAnonymousContext->TryDispatch(DuplicateAnonymousDispatcher, Error));

	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> ConcurrentClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 20), 20.0);
	FUnrealAICredentialFreshnessSource ConcurrentCredentialFreshness;
	FUnrealAISecretValue ConcurrentSecret;
	TestTrue(TEXT("Concurrent credential context fixture secret constructs"),
				  MakeSecret(TArray<uint8>{7, 8, 9}, ConcurrentSecret, Error));
	FUnrealAICredentialLease ConcurrentLease;
	const FUnrealAICredentialDestination ConcurrentBinding = BuildApiKeyDestination(Error);
	TestTrue(TEXT("Concurrent credential context fixture lease constructs"),
				  FUnrealAICredentialLease::TryCreate(ConcurrentBinding, ConcurrentClock,
													  ConcurrentCredentialFreshness.GetToken(), 30.0, {},
													  MoveTemp(ConcurrentSecret), ConcurrentLease, Error));
	const TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> ConcurrentCredentialContext =
		IUnrealAIProviderAccessContext::CreateCredentialed(MoveTemp(ConcurrentLease));
	TAtomic<int32> CredentialDispatchWinners{0};
	TAtomic<int32> CredentialByteWitnesses{0};
	ParallelFor(64,
				[&ConcurrentCredentialContext, &ConcurrentBinding, &CredentialDispatchWinners,
				 &CredentialByteWitnesses](const int32)
				{
					FString LocalError;
					FTestCredentialApplicator Dispatcher(ConcurrentBinding);
					if (ConcurrentCredentialContext->TryDispatch(Dispatcher, LocalError))
					{
						++CredentialDispatchWinners;
						if (Dispatcher.ObservedCount == 3 && Dispatcher.ObservedSum == 24)
						{
							++CredentialByteWitnesses;
						}
					}
				});
	TestEqual(TEXT("Concurrent credential access has exactly one dispatch winner"), CredentialDispatchWinners.Load(),
				   1);
	TestEqual(TEXT("Exactly one credential dispatcher observes the one-shot bytes"), CredentialByteWitnesses.Load(), 1);
	TestFalse(TEXT("Consumed credential access context becomes invalid"), ConcurrentCredentialContext->IsValid());

	FUnrealAICredentialFreshnessSource ConcurrentAnonymousFreshness;
	const TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> ConcurrentAnonymousContext =
		IUnrealAIProviderAccessContext::CreateAnonymous(AnonymousBinding, ConcurrentAnonymousFreshness.GetToken());
	TAtomic<int32> AnonymousDispatchWinners{0};
	ParallelFor(64,
				[&ConcurrentAnonymousContext, &AnonymousBinding, &AnonymousDispatchWinners](const int32)
				{
					FString LocalError;
					FTestCredentialApplicator Dispatcher(AnonymousBinding);
					if (ConcurrentAnonymousContext->TryDispatch(Dispatcher, LocalError) &&
						Dispatcher.bAnonymousDispatched)
					{
						++AnonymousDispatchWinners;
					}
				});
	TestEqual(TEXT("Concurrent anonymous access has exactly one dispatch winner"), AnonymousDispatchWinners.Load(), 1);

	FUnrealAICredentialFreshnessSource StaleAnonymousFreshness;
	const TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> StaleAnonymousContext =
		IUnrealAIProviderAccessContext::CreateAnonymous(AnonymousBinding, StaleAnonymousFreshness.GetToken());
	TestTrue(TEXT("Anonymous route freshness invalidates"), StaleAnonymousFreshness.Invalidate());
	FTestCredentialApplicator StaleAnonymousDispatcher(AnonymousBinding);
	TestFalse(TEXT("Invalidated anonymous access fails before transport admission"),
				   StaleAnonymousContext->TryDispatch(StaleAnonymousDispatcher, Error));
	TestFalse(TEXT("Invalidated anonymous route admits no transport work"),
				   StaleAnonymousDispatcher.bAnonymousDispatched);

	FUnrealAICredentialFreshnessSource ShutdownAnonymousFreshness;
	const TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> ShutdownAnonymousContext =
		IUnrealAIProviderAccessContext::CreateAnonymous(AnonymousBinding, ShutdownAnonymousFreshness.GetToken());
	TestTrue(TEXT("Anonymous route freshness begins shutdown"), ShutdownAnonymousFreshness.BeginShutdown());
	FTestCredentialApplicator ShutdownAnonymousDispatcher(AnonymousBinding);
	TestFalse(TEXT("Shutdown invalidates anonymous provider access"),
				   ShutdownAnonymousContext->TryDispatch(ShutdownAnonymousDispatcher, Error));

	FUnrealAICredentialFreshnessSource BlockingAnonymousFreshness;
	const TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> BlockingAnonymousContext =
		IUnrealAIProviderAccessContext::CreateAnonymous(AnonymousBinding, BlockingAnonymousFreshness.GetToken());
	FBlockingAnonymousApplicator BlockingAnonymousDispatcher(AnonymousBinding);
	FString BlockingAnonymousError;
	TFuture<bool> BlockingAnonymousFuture =
		Async(EAsyncExecution::ThreadPool,
			  [&BlockingAnonymousContext, &BlockingAnonymousDispatcher, &BlockingAnonymousError]()
			  { return BlockingAnonymousContext->TryDispatch(BlockingAnonymousDispatcher, BlockingAnonymousError); });
	TestTrue(TEXT("Anonymous dispatch reaches transport admission while holding freshness permit"),
				  BlockingAnonymousDispatcher.WaitUntilEntered(2000));
	TestFalse(TEXT("Anonymous invalidation never blocks behind active transport admission"),
				   BlockingAnonymousFreshness.Invalidate());
	BlockingAnonymousDispatcher.Release();
	TestTrue(TEXT("Admitted anonymous transport completes after deterministic release"),
				  BlockingAnonymousFuture.WaitFor(FTimespan::FromSeconds(2.0)));
	if (BlockingAnonymousFuture.IsReady())
	{
		TestTrue(TEXT("Anonymous dispatch linearized before deferred invalidation"), BlockingAnonymousFuture.Get());
	}
	TestTrue(TEXT("Deferred anonymous invalidation settles after admission releases"),
				  BlockingAnonymousFreshness.Invalidate());

	FUnrealAICredentialFreshnessSource ExpiredFactoryFreshness;
	FUnrealAISecretValue ExpiredFactorySecret;
	TestTrue(TEXT("Expired context factory fixture secret constructs"),
				  MakeSecret(TArray<uint8>{1, 2, 3}, ExpiredFactorySecret, Error));
	FUnrealAICredentialLease ExpiredFactoryLease;
	TestTrue(TEXT("Expired context factory fixture lease constructs"),
				  FUnrealAICredentialLease::TryCreate(ConcurrentBinding, ConcurrentClock,
													  ExpiredFactoryFreshness.GetToken(), 1.0, {},
													  MoveTemp(ExpiredFactorySecret), ExpiredFactoryLease, Error));
	ConcurrentClock->Advance(FTimespan::FromSeconds(1.0));
	TestFalse(TEXT("Credential context factory rejects an expired lease"),
				   IUnrealAIProviderAccessContext::CreateCredentialed(MoveTemp(ExpiredFactoryLease)).IsValid());
	TestTrue(TEXT("Rejected expired context factory input is wiped immediately"),
				  ExpiredFactoryLease.GetRedactedDisplay().Contains(TEXT("unset")));

	FUnrealAICredentialFreshnessSource RevokedFactoryFreshness;
	FUnrealAISecretValue RevokedFactorySecret;
	TestTrue(TEXT("Revoked context factory fixture secret constructs"),
				  MakeSecret(TArray<uint8>{4, 5, 6}, RevokedFactorySecret, Error));
	FUnrealAICredentialLease RevokedFactoryLease;
	TestTrue(TEXT("Revoked context factory fixture lease constructs"),
				  FUnrealAICredentialLease::TryCreate(ConcurrentBinding, ConcurrentClock,
													  RevokedFactoryFreshness.GetToken(), 30.0, {},
													  MoveTemp(RevokedFactorySecret), RevokedFactoryLease, Error));
	TestTrue(TEXT("Revoked context factory freshness invalidates"), RevokedFactoryFreshness.Invalidate());
	TestFalse(TEXT("Credential context factory rejects a revoked lease"),
				   IUnrealAIProviderAccessContext::CreateCredentialed(MoveTemp(RevokedFactoryLease)).IsValid());
	TestTrue(TEXT("Rejected revoked context factory input is wiped immediately"),
				  RevokedFactoryLease.GetRedactedDisplay().Contains(TEXT("unset")));
	return true;
}
