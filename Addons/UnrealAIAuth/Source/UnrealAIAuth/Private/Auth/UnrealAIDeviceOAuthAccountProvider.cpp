// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIDeviceOAuthAccountProvider.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

namespace
{
using namespace UE::UnrealAI::Private;

FUnrealAIProviderAccessError MakeAuthError(const EUnrealAIErrorCategory Category,
										   const EUnrealAIProviderAccessErrorCode Code, const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

FUnrealAIProviderAccessError NormalizeAuthFailure(const FUnrealAIProviderAccessError &Candidate)
{
	FString ShapeError;
	if (!Candidate.ValidateShape(ShapeError))
	{
		return MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed,
							 Candidate.bRetryable);
	}
	switch (Candidate.Code)
	{
	case EUnrealAIProviderAccessErrorCode::AuthCancelled:
	case EUnrealAIProviderAccessErrorCode::AuthTimedOut:
	case EUnrealAIProviderAccessErrorCode::AuthFailed:
	case EUnrealAIProviderAccessErrorCode::AuthResponseInvalid:
	case EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete:
	case EUnrealAIProviderAccessErrorCode::AuthScopeInsufficient:
	case EUnrealAIProviderAccessErrorCode::AuthTokenTypeUnsupported:
	case EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid:
	case EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed:
	case EUnrealAIProviderAccessErrorCode::PartnerGated:
	case EUnrealAIProviderAccessErrorCode::AccessProfileNotReady:
	case EUnrealAIProviderAccessErrorCode::UnsupportedCapability:
		return Candidate;
	default:
		return MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed,
							 Candidate.bRetryable);
	}
}
} // namespace

namespace UE::UnrealAI::Private
{
class FDeviceOAuthAuthOperation final : public IUnrealAIAuthOperationHandle,
										public IUnrealAIDeviceOAuthInteractionPublisher,
										public TSharedFromThis<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe>
{
  public:
	FDeviceOAuthAuthOperation(
		const FUnrealAIRequestId InRequestId, const FName InAuthProfileId, const FUnrealAIAccessAccountId &InAccountId,
		const EUnrealAIAuthOperationKind InOperationKind, const EUnrealAIAccountAuthState InCancelledState,
		const EUnrealAIAccountAuthState InFailureState, const double TimeoutSeconds,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> InSink,
		const FUnrealAICancellationToken &ParentCancellation,
		TFunction<void(const FUnrealAIRequestId &, EUnrealAIAuthEventKind, EUnrealAIAccountAuthState)> InOnTerminal)
		: RequestId(InRequestId), AuthProfileId(InAuthProfileId), AccountId(InAccountId),
		  OperationKind(InOperationKind), CancelledState(InCancelledState), FailureState(InFailureState),
		  Clock(MoveTemp(InClock)), Cancellation(ParentCancellation),
		  Deadline(FUnrealAIDeadline::FromNow(*Clock, TimeoutSeconds)), Sink(InSink.ToSharedPtr()),
		  OnTerminal(MoveTemp(InOnTerminal))
	{
	}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}

	void Cancel() override
	{
		Cancellation.Cancel(EUnrealAICancellationReason::Requested);
		PublishTerminal(
			EUnrealAIAuthEventKind::Cancelled, CancelledState,
			MakeAuthError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled));
	}

	bool PublishInteraction(FUnrealAIAuthInteraction &&Interaction) override
	{
		if (OperationKind != EUnrealAIAuthOperationKind::SignIn)
		{
			Interaction.Reset();
			return false;
		}
		FUnrealAIAuthEvent Event;
		Event.RequestId = RequestId;
		Event.AuthProfileId = AuthProfileId;
		Event.AccountId = AccountId;
		Event.OperationKind = OperationKind;
		Event.Kind = EUnrealAIAuthEventKind::InteractionRequired;
		Event.State = EUnrealAIAccountAuthState::Authorizing;
		Event.Interaction = MakeUnique<FUnrealAIAuthInteraction>(MoveTemp(Interaction));
		FString ShapeError;
		if (!Event.ValidateShape(ShapeError))
		{
			Event.Interaction.Reset();
			PublishTerminal(
				EUnrealAIAuthEventKind::Failed, FailureState,
				MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed));
			return false;
		}
		bool bShouldDrain = false;
		{
			FScopeLock Lock(&SinkMutex);
			if (Terminal.IsComplete() || !Sink.IsValid() || bInteractionPublished.Load())
			{
				Event.Interaction.Reset();
				return false;
			}
			bInteractionPublished.Store(true);
			PendingEvents.Add(MoveTemp(Event));
			if (!bDrainingEvents)
			{
				bDrainingEvents = true;
				bShouldDrain = true;
			}
		}
		if (bShouldDrain)
		{
			DrainQueuedEvents();
		}
		return true;
	}

	const FUnrealAICancellationToken GetCancellationToken() const
	{
		return Cancellation.GetToken();
	}

	double RemainingSeconds() const
	{
		return Deadline.RemainingSeconds(*Clock);
	}

	bool IsComplete() const
	{
		return Terminal.IsComplete();
	}

	bool ObserveLogicalTerminal()
	{
		if (Terminal.IsComplete())
		{
			return true;
		}
		const FUnrealAICancellationToken Token = Cancellation.GetToken();
		if (Token.IsCancellationRequested())
		{
			if (Token.GetReason() == EUnrealAICancellationReason::Timeout)
			{
				PublishTerminal(EUnrealAIAuthEventKind::TimedOut, FailureState,
								MakeAuthError(EUnrealAIErrorCategory::Timeout,
											  EUnrealAIProviderAccessErrorCode::AuthTimedOut, true));
			}
			else
			{
				PublishTerminal(
					EUnrealAIAuthEventKind::Cancelled, CancelledState,
					MakeAuthError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled));
			}
			return true;
		}
		if (Deadline.IsExpired(*Clock))
		{
			Cancellation.Cancel(EUnrealAICancellationReason::Timeout);
			PublishTerminal(
				EUnrealAIAuthEventKind::TimedOut, FailureState,
				MakeAuthError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::AuthTimedOut, true));
			return true;
		}
		return false;
	}

	void Monitor()
	{
		while (!Terminal.IsComplete())
		{
			ObserveLogicalTerminal();
			if (!Terminal.IsComplete())
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
		}
	}

	bool PublishSucceeded(const EUnrealAIAccountAuthState State)
	{
		return PublishTerminal(EUnrealAIAuthEventKind::Succeeded, State, {});
	}

	bool TryClaimSucceeded()
	{
		return Terminal.TryComplete(EUnrealAITerminalKind::Succeeded);
	}

	void PublishClaimedSucceeded(const EUnrealAIAccountAuthState State)
	{
		PublishClaimedTerminal(EUnrealAIAuthEventKind::Succeeded, State, {});
	}

	bool PublishFailed(const FUnrealAIProviderAccessError &Error)
	{
		const FUnrealAIProviderAccessError Normalized = NormalizeAuthFailure(Error);
		if (Normalized.Category == EUnrealAIErrorCategory::Cancelled)
		{
			return PublishTerminal(EUnrealAIAuthEventKind::Cancelled, CancelledState, Normalized);
		}
		if (Normalized.Category == EUnrealAIErrorCategory::Timeout)
		{
			return PublishTerminal(EUnrealAIAuthEventKind::TimedOut, FailureState, Normalized);
		}
		return PublishTerminal(EUnrealAIAuthEventKind::Failed, FailureState, Normalized);
	}

  private:
	bool PublishTerminal(const EUnrealAIAuthEventKind Kind, const EUnrealAIAccountAuthState State,
						 const FUnrealAIProviderAccessError &Error)
	{
		EUnrealAITerminalKind TerminalKind = EUnrealAITerminalKind::Failed;
		if (Kind == EUnrealAIAuthEventKind::Succeeded)
		{
			TerminalKind = EUnrealAITerminalKind::Succeeded;
		}
		else if (Kind == EUnrealAIAuthEventKind::Cancelled)
		{
			TerminalKind = EUnrealAITerminalKind::Cancelled;
		}
		else if (Kind == EUnrealAIAuthEventKind::TimedOut)
		{
			TerminalKind = EUnrealAITerminalKind::TimedOut;
		}
		if (!Terminal.TryComplete(TerminalKind))
		{
			return false;
		}
		PublishClaimedTerminal(Kind, State, Error);
		return true;
	}

	void PublishClaimedTerminal(const EUnrealAIAuthEventKind Kind, const EUnrealAIAccountAuthState State,
								const FUnrealAIProviderAccessError &Error)
	{
		FUnrealAIAuthEvent Event;
		Event.RequestId = RequestId;
		Event.AuthProfileId = AuthProfileId;
		Event.AccountId = AccountId;
		Event.OperationKind = OperationKind;
		Event.Kind = Kind;
		Event.State = State;
		Event.Error = Error;
		FString ShapeError;
		if (!Event.ValidateShape(ShapeError))
		{
			Event.Kind = EUnrealAIAuthEventKind::Failed;
			Event.State = EUnrealAIAccountAuthState::Failed;
			Event.Error = MakeAuthError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
		}
		if (OnTerminal)
		{
			OnTerminal(RequestId, Kind, State);
		}
		QueueEvent(MoveTemp(Event));
	}

	void QueueEvent(FUnrealAIAuthEvent &&Event)
	{
		bool bShouldDrain = false;
		{
			FScopeLock Lock(&SinkMutex);
			if (!Sink.IsValid())
			{
				return;
			}
			PendingEvents.Add(MoveTemp(Event));
			if (!bDrainingEvents)
			{
				bDrainingEvents = true;
				bShouldDrain = true;
			}
		}
		if (bShouldDrain)
		{
			DrainQueuedEvents();
		}
	}

	void DrainQueuedEvents()
	{
		for (;;)
		{
			FUnrealAIAuthEvent Event;
			TSharedPtr<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> LocalSink;
			{
				FScopeLock Lock(&SinkMutex);
				if (PendingEvents.IsEmpty())
				{
					bDrainingEvents = false;
					return;
				}
				Event = MoveTemp(PendingEvents[0]);
				PendingEvents.RemoveAt(0, 1, EAllowShrinking::No);
				LocalSink = Sink;
				if (Event.IsTerminal())
				{
					Sink.Reset();
				}
			}
			if (LocalSink.IsValid())
			{
				LocalSink->EnqueueAuthEvent(MoveTemp(Event));
			}
		}
	}

	FUnrealAIRequestId RequestId;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	EUnrealAIAuthOperationKind OperationKind = EUnrealAIAuthOperationKind::Invalid;
	EUnrealAIAccountAuthState CancelledState = EUnrealAIAccountAuthState::SignedOut;
	EUnrealAIAccountAuthState FailureState = EUnrealAIAccountAuthState::Failed;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAICancellationSource Cancellation;
	FUnrealAIDeadline Deadline;
	mutable FCriticalSection SinkMutex;
	TSharedPtr<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink;
	TArray<FUnrealAIAuthEvent> PendingEvents;
	TFunction<void(const FUnrealAIRequestId &, EUnrealAIAuthEventKind, EUnrealAIAccountAuthState)> OnTerminal;
	TAtomic<bool> bInteractionPublished{false};
	bool bDrainingEvents = false;
	FUnrealAITerminalGuard Terminal;
};

class FDeviceOAuthAccountProviderState final
	: public TSharedFromThis<FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe>
{
  public:
	FDeviceOAuthAccountProviderState(const FUnrealAIDeviceOAuthAccountProviderConfig &InConfig,
									 TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
									 TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
									 TSharedRef<IUnrealAIDeviceOAuthAuthorizationDriver, ESPMode::ThreadSafe> InDriver)
		: Config(InConfig), SecretStore(MoveTemp(InSecretStore)), Clock(MoveTemp(InClock)), Driver(MoveTemp(InDriver))
	{
		FString ConfigError;
		const FUnrealAISecretStoreCapabilities Capabilities = SecretStore->DescribeCapabilities();
		FString CapabilityError;
		bShutdown = !Config.ValidateShape(ConfigError) || Config.ProviderName != Driver->GetProviderName() ||
					Config.SecretHandle.StoreName != SecretStore->GetStoreName() ||
					!Capabilities.ValidateShape(CapabilityError) || !Capabilities.bAvailableInCurrentBuild ||
					Capabilities.PersistenceClass != EUnrealAISecretStorePersistenceClass::Persistent ||
					!Capabilities.IsProductionProtected() || !Capabilities.bAtomicCompareAndSwap;
#if UE_BUILD_SHIPPING
		bShutdown = bShutdown || !Capabilities.bAvailableInShipping;
#endif
#if UE_BUILD_SHIPPING && UE_SERVER
		bShutdown = bShutdown ||
					Capabilities.ProtectionClass != EUnrealAISecretStoreProtectionClass::ExternalSecretService ||
					Capabilities.ScopeClass != EUnrealAISecretStoreScopeClass::Service;
#endif
		Status.ProviderName = Config.ProviderName;
		Status.AuthProfileId = Config.AuthProfileId;
		Status.AccountId = Config.AccountId;
		Status.State = EUnrealAIAccountAuthState::SignedOut;
	}

	bool SetCredentialBroker(TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InBroker,
							 FString &OutError)
	{
		OutError.Reset();
		FScopeLock Lock(&Mutex);
		if (bShutdown || Broker.IsValid())
		{
			OutError = bShutdown ? TEXT("Device OAuth account provider is shut down.")
								 : TEXT("Device OAuth account provider already has a credential broker.");
			return false;
		}
		Broker = InBroker.ToSharedPtr();
		return true;
	}

	void StartStoredSessionRecovery()
	{
		uint64 RecoveryEpoch = 0;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || bRecoveryStarted || bRecoveryInFlight || !ActiveOperations.IsEmpty() ||
				!PhysicalOperationIds.IsEmpty())
			{
				return;
			}
			bRecoveryStarted = true;
			bRecoveryInFlight = true;
			RecoveryEpoch = SessionEpoch;
			Status.State = EUnrealAIAccountAuthState::Refreshing;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = false;
		}
		const TSharedRef<FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe> Self = AsShared();
		(void)Async(EAsyncExecution::Thread,
					[Self, RecoveryEpoch]()
					{
						Self->RecoverStoredSession(RecoveryEpoch);
						Self->OnRecoveryPhysicalComplete();
					});
	}

	FUnrealAIAccountStatus GetStatus(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) const
	{
		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		FUnrealAIAccountStatus Result;
		bool bLocalSignedOutConfirmed = false;
		bool bLocalQuarantined = false;
		{
			FScopeLock Lock(&Mutex);
			if (AuthProfileId != Config.AuthProfileId || !(AccountId == Config.AccountId))
			{
				FUnrealAIAccountStatus Invalid;
				Invalid.ProviderName = Config.ProviderName;
				Invalid.AuthProfileId = AuthProfileId;
				Invalid.AccountId = AccountId;
				Invalid.State = EUnrealAIAccountAuthState::Invalid;
				return Invalid;
			}
			Result = Status;
			LocalBroker = Broker.Pin();
			bLocalSignedOutConfirmed = bConfirmedSignedOut;
			bLocalQuarantined = bLocalReauthenticationQuarantine;
		}
		FUnrealAIProviderAccessError Quarantine;
		const bool bBrokerQuarantined =
			LocalBroker.IsValid() && LocalBroker->TryGetAccountQuarantine(AuthProfileId, AccountId, Quarantine);
		if ((bLocalQuarantined || bBrokerQuarantined) && Result.State != EUnrealAIAccountAuthState::Authorizing &&
			Result.State != EUnrealAIAccountAuthState::Refreshing &&
			Result.State != EUnrealAIAccountAuthState::Revoking &&
			!(Result.State == EUnrealAIAccountAuthState::SignedOut && bLocalSignedOutConfirmed))
		{
			Result.State = EUnrealAIAccountAuthState::ReauthenticationRequired;
			Result.AccessExpiresAtUtc.Reset();
			Result.bRefreshCredentialPresent = false;
		}
		// Signed-out status is intentionally unlinkable. The exact local account remains in the catalog/request
		// binding, while public status exposes it only after an account-bearing lifecycle state exists.
		Result.AccountId =
			Result.State == EUnrealAIAccountAuthState::SignedOut ? FUnrealAIAccessAccountId{} : Config.AccountId;
		return Result;
	}

	bool StartSignIn(const FUnrealAIInteractiveAuthRequest &Request,
					 TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
					 const FUnrealAICancellationToken &Cancellation,
					 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					 FUnrealAIProviderAccessError &OutError)
	{
		OutHandle.Reset();
		OutError = {};
		FString ShapeError;
		if (!Request.ValidateShape(ShapeError) || Request.Flow != EUnrealAIInteractiveAuthFlow::DeviceCode ||
			Request.AuthProfileId != Config.AuthProfileId || !Cancellation.IsValid())
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::InvalidArgument,
									 EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}
		const EUnrealAIAccountAuthState EffectiveState = GetStatus(Request.AuthProfileId, Config.AccountId).State;
		if (EffectiveState == EUnrealAIAccountAuthState::Ready ||
			EffectiveState == EUnrealAIAccountAuthState::Refreshing ||
			EffectiveState == EUnrealAIAccountAuthState::Revoking ||
			EffectiveState == EUnrealAIAccountAuthState::Authorizing)
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::OperationBusy);
			return false;
		}
		const bool bReauthentication = EffectiveState == EUnrealAIAccountAuthState::ReauthenticationRequired;
		const EUnrealAIAccountAuthState CancelledState = bReauthentication
															 ? EUnrealAIAccountAuthState::ReauthenticationRequired
															 : EUnrealAIAccountAuthState::SignedOut;
		const EUnrealAIAccountAuthState FailureState =
			bReauthentication ? EUnrealAIAccountAuthState::ReauthenticationRequired : EUnrealAIAccountAuthState::Failed;
		uint64 OperationEpoch = 0;
		const TWeakPtr<FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe> WeakSelf = AsShared();
		const TSharedRef<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe> Operation =
			MakeShared<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe>(
				Request.RequestId, Request.AuthProfileId, Config.AccountId, EUnrealAIAuthOperationKind::SignIn,
				CancelledState, FailureState, Request.TimeoutSeconds, Clock, Sink, Cancellation,
				[WeakSelf](const FUnrealAIRequestId &CompletedRequest, const EUnrealAIAuthEventKind Kind,
						   const EUnrealAIAccountAuthState State)
				{
					if (const TSharedPtr<FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin())
					{
						Pinned->OnOperationTerminal(CompletedRequest, Kind, State);
					}
				});
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || bRecoveryInFlight || PhysicalOperationIds.Num() >= 1 ||
				PhysicalOperationIds.Contains(Request.RequestId.Value) ||
				Status.State == EUnrealAIAccountAuthState::Refreshing ||
				Status.State == EUnrealAIAccountAuthState::Revoking ||
				Status.State == EUnrealAIAccountAuthState::Authorizing ||
				(Status.State == EUnrealAIAccountAuthState::Ready && !bReauthentication))
			{
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::OperationBusy, true);
				return false;
			}
			OperationEpoch = ++SessionEpoch;
			Status.State = EUnrealAIAccountAuthState::Authorizing;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = false;
			ActiveOperations.Add(Request.RequestId.Value, Operation);
			PhysicalOperationIds.Add(Request.RequestId.Value);
		}
		OutHandle = Operation;
		const TSharedRef<FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe> Self = AsShared();
		(void)Async(EAsyncExecution::Thread,
					[Self, Operation, OperationEpoch]()
					{
						Self->RunSignIn(Operation, OperationEpoch);
						Self->OnPhysicalOperationComplete(Operation->GetRequestId());
					});
		(void)Async(EAsyncExecution::Thread, [Operation]() { Operation->Monitor(); });
		return true;
	}

	bool StartSignOut(const FUnrealAIAccountAuthRequest &Request,
					  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError)
	{
		OutHandle.Reset();
		OutError = {};
		FString ShapeError;
		if (!Request.ValidateShape(ShapeError) || Request.AuthProfileId != Config.AuthProfileId ||
			!(Request.AccountId == Config.AccountId) || !Cancellation.IsValid())
		{
			OutError = MakeAuthError(EUnrealAIErrorCategory::InvalidArgument,
									 EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError =
				MakeAuthError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}
		uint64 OperationEpoch = 0;
		const TWeakPtr<FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe> WeakSelf = AsShared();
		const TSharedRef<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe> Operation =
			MakeShared<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe>(
				Request.RequestId, Request.AuthProfileId, Request.AccountId, EUnrealAIAuthOperationKind::SignOut,
				EUnrealAIAccountAuthState::ReauthenticationRequired,
				EUnrealAIAccountAuthState::ReauthenticationRequired, Request.TimeoutSeconds, Clock, Sink, Cancellation,
				[WeakSelf](const FUnrealAIRequestId &CompletedRequest, const EUnrealAIAuthEventKind Kind,
						   const EUnrealAIAccountAuthState State)
				{
					if (const TSharedPtr<FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin())
					{
						Pinned->OnOperationTerminal(CompletedRequest, Kind, State);
					}
				});
		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || bRecoveryInFlight || PhysicalOperationIds.Num() >= 1 ||
				PhysicalOperationIds.Contains(Request.RequestId.Value))
			{
				OutError =
					MakeAuthError(EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::OperationBusy, true);
				return false;
			}
			OperationEpoch = ++SessionEpoch;
			Status.State = EUnrealAIAccountAuthState::Revoking;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = false;
			ActiveOperations.Add(Request.RequestId.Value, Operation);
			PhysicalOperationIds.Add(Request.RequestId.Value);
			LocalBroker = Broker.Pin();
		}
		if (LocalBroker.IsValid())
		{
			LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
		}
		OutHandle = Operation;
		const TSharedRef<FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe> Self = AsShared();
		(void)Async(EAsyncExecution::Thread,
					[Self, Operation, OperationEpoch]()
					{
						Self->RunSignOut(Operation, OperationEpoch);
						Self->OnPhysicalOperationComplete(Operation->GetRequestId());
					});
		(void)Async(EAsyncExecution::Thread, [Operation]() { Operation->Monitor(); });
		return true;
	}

	void BeginShutdown()
	{
		TArray<TSharedPtr<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe>> Pending;
		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown)
			{
				return;
			}
			bShutdown = true;
			++SessionEpoch;
			ActiveOperations.GenerateValueArray(Pending);
			LocalBroker = Broker.Pin();
			Broker.Reset();
			Status.State = EUnrealAIAccountAuthState::SignedOut;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = false;
		}
		if (LocalBroker.IsValid())
		{
			LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
		}
		for (const TSharedPtr<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe> &Operation : Pending)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
	}

	int32 GetActiveOperationCount() const
	{
		FScopeLock Lock(&Mutex);
		return ActiveOperations.Num();
	}

	int32 GetPhysicalOperationCount() const
	{
		FScopeLock Lock(&Mutex);
		return PhysicalOperationIds.Num() + (bRecoveryInFlight ? 1 : 0);
	}

	bool IsShutdown() const
	{
		FScopeLock Lock(&Mutex);
		return bShutdown;
	}

	const FUnrealAIDeviceOAuthAccountProviderConfig &GetConfig() const
	{
		return Config;
	}

  private:
	void RunSignIn(const TSharedRef<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe> &Operation,
				   const uint64 OperationEpoch)
	{
		if (Operation->ObserveLogicalTerminal() || !IsEpochCurrent(OperationEpoch))
		{
			return;
		}
		FUnrealAIOAuthTokenSet Tokens;
		FUnrealAIProviderAccessError DriverError;
		if (!Driver->Authorize(Operation->RemainingSeconds(), Operation->GetCancellationToken(), *Operation, Tokens,
							   DriverError))
		{
			Tokens.Reset();
			if (!Operation->ObserveLogicalTerminal())
			{
				PublishSignInFailure(Operation, DriverError);
			}
			return;
		}
		if (Operation->ObserveLogicalTerminal() || !IsEpochCurrent(OperationEpoch))
		{
			Tokens.Reset();
			return;
		}
		if (!Tokens.RefreshToken.IsSet() || (Config.bRequiresProtectedSecondary && !Tokens.AccountRoutingValue.IsSet()))
		{
			Tokens.Reset();
			PublishSignInFailure(Operation, MakeAuthError(EUnrealAIErrorCategory::Provider,
														  EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete));
			return;
		}
		const FDateTime Expiry = Tokens.AccessTokenExpiresAtUtc;
		const bool bRefreshPresent = Tokens.RefreshToken.IsSet();
		FUnrealAIOAuthTokenEnvelope Envelope;
		FString EnvelopeError;
		const FUnrealAIOAuthTokenEnvelopeBinding Binding{Config.ProviderName, Config.AuthProfileId, Config.AccountId};
		if (!FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Binding, MoveTemp(Tokens), Clock->UtcNow(), Envelope,
														 EnvelopeError))
		{
			const bool bExpiryInvalid = Expiry.GetTicks() % ETimespan::TicksPerSecond != 0 || Expiry <= Clock->UtcNow();
			PublishSignInFailure(Operation,
								 MakeAuthError(EUnrealAIErrorCategory::Provider,
											   bExpiryInvalid ? EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid
															  : EUnrealAIProviderAccessErrorCode::AuthResponseInvalid));
			return;
		}
		FUnrealAISecretValue Encoded;
		if (!FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(Envelope), Clock->UtcNow(), Encoded, EnvelopeError))
		{
			PublishSignInFailure(Operation, MakeAuthError(EUnrealAIErrorCategory::Provider,
														  EUnrealAIProviderAccessErrorCode::AuthResponseInvalid));
			return;
		}
		FUnrealAISecretStoreOperationContext StoreContext;
		if (!TryMakeStoreContext(*Operation, StoreContext))
		{
			Encoded.Reset();
			Operation->ObserveLogicalTerminal();
			if (!Operation->IsComplete())
			{
				PublishSignInFailure(Operation, MakeAuthError(EUnrealAIErrorCategory::Provider,
															  EUnrealAIProviderAccessErrorCode::AuthFailed));
			}
			return;
		}
		FUnrealAISecretValue Existing;
		uint64 ExistingRevision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult LoadResult =
			SecretStore->Load(StoreContext, Config.SecretHandle, Existing, ExistingRevision, StoreError);
		const bool bExistingLoadShapeValid =
			(LoadResult == EUnrealAISecretStoreResult::Succeeded && ExistingRevision != 0 && Existing.IsSet()) ||
			(LoadResult == EUnrealAISecretStoreResult::NotFound && ExistingRevision == 0 && !Existing.IsSet());
		Existing.Reset();
		if (Operation->ObserveLogicalTerminal() || !IsEpochCurrent(OperationEpoch))
		{
			Encoded.Reset();
			return;
		}
		if (!bExistingLoadShapeValid)
		{
			Encoded.Reset();
			PublishSignInFailure(Operation,
								 MakeAuthError(EUnrealAIErrorCategory::Persistence,
											   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true));
			return;
		}
		uint64 NewRevision = 0;
		const EUnrealAISecretStoreResult StoreResult = SecretStore->Store(
			StoreContext, Config.SecretHandle, Encoded,
			LoadResult == EUnrealAISecretStoreResult::Succeeded ? ExistingRevision : 0, NewRevision, StoreError);
		if (StoreResult != EUnrealAISecretStoreResult::Succeeded || NewRevision == 0)
		{
			if (StoreResult == EUnrealAISecretStoreResult::Cancelled ||
				StoreResult == EUnrealAISecretStoreResult::TimedOut ||
				(StoreResult == EUnrealAISecretStoreResult::Succeeded && NewRevision == 0))
			{
				ReconcileAmbiguousStore(LoadResult == EUnrealAISecretStoreResult::Succeeded ? ExistingRevision : 0,
										Encoded);
			}
			Encoded.Reset();
			if (!Operation->ObserveLogicalTerminal())
			{
				PublishSignInFailure(Operation,
									 MakeAuthError(EUnrealAIErrorCategory::Persistence,
												   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed,
												   StoreError.bRetryable ||
													   StoreResult == EUnrealAISecretStoreResult::TimedOut ||
													   StoreResult == EUnrealAISecretStoreResult::Unavailable ||
													   StoreResult == EUnrealAISecretStoreResult::Locked));
			}
			return;
		}
		if (Operation->ObserveLogicalTerminal() || !IsEpochCurrent(OperationEpoch))
		{
			CompensateStaleStore(NewRevision, Encoded);
			Encoded.Reset();
			return;
		}
		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		bool bStaleBeforeNotification = false;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || SessionEpoch != OperationEpoch)
			{
				bStaleBeforeNotification = true;
			}
			else
			{
				LocalBroker = Broker.Pin();
			}
		}
		if (bStaleBeforeNotification)
		{
			CompensateStaleStore(NewRevision, Encoded);
			Encoded.Reset();
			return;
		}
		if (LocalBroker.IsValid())
		{
			FString ReplacementError;
			if (!LocalBroker->NotifyCredentialReplaced(Config.AuthProfileId, Config.AccountId, NewRevision,
													   Operation->RemainingSeconds(), Operation->GetCancellationToken(),
													   ReplacementError))
			{
				LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
				CompensateStaleStore(NewRevision, Encoded);
				Encoded.Reset();
				PublishSignInFailure(Operation, MakeAuthError(EUnrealAIErrorCategory::Persistence,
															  EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed));
				return;
			}
		}
		if (Operation->ObserveLogicalTerminal() || !IsEpochCurrent(OperationEpoch))
		{
			if (LocalBroker.IsValid())
			{
				LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
			}
			CompensateStaleStore(NewRevision, Encoded);
			Encoded.Reset();
			return;
		}
		bool bSuccessClaimed = false;
		{
			FScopeLock Lock(&Mutex);
			if (!bShutdown && SessionEpoch == OperationEpoch && Operation->TryClaimSucceeded())
			{
				Status.State = EUnrealAIAccountAuthState::Ready;
				Status.AccessExpiresAtUtc = Expiry;
				Status.bRefreshCredentialPresent = bRefreshPresent;
				bConfirmedSignedOut = false;
				bLocalReauthenticationQuarantine = false;
				ActiveOperations.Remove(Operation->GetRequestId().Value);
				bSuccessClaimed = true;
			}
		}
		if (!bSuccessClaimed)
		{
			if (LocalBroker.IsValid())
			{
				LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
			}
			CompensateStaleStore(NewRevision, Encoded);
			Encoded.Reset();
			return;
		}
		Encoded.Reset();
		Operation->PublishClaimedSucceeded(EUnrealAIAccountAuthState::Ready);
	}

	void RunSignOut(const TSharedRef<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe> &Operation,
					const uint64 OperationEpoch)
	{
		// Sign-out cleanup owns an independent bounded lineage. Logical cancellation/timeout ends UI delivery but must
		// not abandon a credential that may still be present in the secure store.
		FUnrealAICancellationSource CleanupCancellation;
		FUnrealAISecretStoreOperationContext StoreContext;
		FString ContextError;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(
				Clock, FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds, CleanupCancellation.GetToken(),
				StoreContext, ContextError))
		{
			SetReauthenticationRequiredIfCurrent(OperationEpoch);
			if (!Operation->IsComplete())
			{
				Operation->PublishFailed(MakeAuthError(EUnrealAIErrorCategory::Persistence,
													   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true));
			}
			return;
		}
		FUnrealAISecretValue Existing;
		uint64 Revision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult LoadResult =
			SecretStore->Load(StoreContext, Config.SecretHandle, Existing, Revision, StoreError);
		Existing.Reset();
		EUnrealAISecretStoreResult DeleteResult = EUnrealAISecretStoreResult::Succeeded;
		if (LoadResult == EUnrealAISecretStoreResult::Succeeded)
		{
			DeleteResult = Revision != 0 ? SecretStore->Delete(StoreContext, Config.SecretHandle, Revision, StoreError)
										 : EUnrealAISecretStoreResult::Corrupt;
		}
		else if (LoadResult != EUnrealAISecretStoreResult::NotFound)
		{
			DeleteResult = LoadResult;
		}
		if (DeleteResult != EUnrealAISecretStoreResult::Succeeded &&
			DeleteResult != EUnrealAISecretStoreResult::NotFound)
		{
			SetReauthenticationRequiredIfCurrent(OperationEpoch);
			if (!Operation->IsComplete())
			{
				Operation->PublishFailed(MakeAuthError(EUnrealAIErrorCategory::Persistence,
													   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true));
			}
			return;
		}
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || SessionEpoch != OperationEpoch)
			{
				return;
			}
			Status.State = EUnrealAIAccountAuthState::SignedOut;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = true;
			bLocalReauthenticationQuarantine = false;
		}
		Operation->PublishSucceeded(EUnrealAIAccountAuthState::SignedOut);
	}

	void RecoverStoredSession(const uint64 RecoveryEpoch)
	{
		FUnrealAICancellationSource Cancellation;
		FUnrealAISecretStoreOperationContext StoreContext;
		FString ContextError;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock,
															 FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds,
															 Cancellation.GetToken(), StoreContext, ContextError))
		{
			FScopeLock Lock(&Mutex);
			if (!bShutdown && SessionEpoch == RecoveryEpoch)
			{
				Status.State = EUnrealAIAccountAuthState::Failed;
				Status.AccessExpiresAtUtc.Reset();
				Status.bRefreshCredentialPresent = false;
				bConfirmedSignedOut = false;
			}
			return;
		}
		FUnrealAISecretValue Encoded;
		uint64 Revision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult Result =
			SecretStore->Load(StoreContext, Config.SecretHandle, Encoded, Revision, StoreError);
		FUnrealAIAccountStatus Recovered;
		Recovered.ProviderName = Config.ProviderName;
		Recovered.AuthProfileId = Config.AuthProfileId;
		Recovered.AccountId = Config.AccountId;
		Recovered.State = EUnrealAIAccountAuthState::SignedOut;
		bool bQuarantineRecovered = false;
		bool bRecoveredSignedOutConfirmed = false;
		if (Result == EUnrealAISecretStoreResult::Succeeded && Revision != 0)
		{
			FUnrealAIOAuthTokenEnvelope Envelope;
			FString DecodeError;
			const FUnrealAIOAuthTokenEnvelopeBinding Binding{Config.ProviderName, Config.AuthProfileId,
															 Config.AccountId};
			if (FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeForRefresh(MoveTemp(Encoded), Binding, Clock->UtcNow(),
																	  Envelope, DecodeError) &&
				Envelope.HasRefreshToken() &&
				(!Config.bRequiresProtectedSecondary || Envelope.HasAccountRoutingValue()))
			{
				Recovered.State = EUnrealAIAccountAuthState::Ready;
				Recovered.AccessExpiresAtUtc = Envelope.GetAccessTokenExpiresAtUtc();
				Recovered.bRefreshCredentialPresent = Envelope.HasRefreshToken();
			}
			else
			{
				Recovered.State = EUnrealAIAccountAuthState::ReauthenticationRequired;
				Recovered.AccessExpiresAtUtc.Reset();
				Recovered.bRefreshCredentialPresent = false;
				bQuarantineRecovered = true;
			}
		}
		else
		{
			Encoded.Reset();
			Recovered.State = Result == EUnrealAISecretStoreResult::NotFound
								  ? EUnrealAIAccountAuthState::SignedOut
								  : (Result == EUnrealAISecretStoreResult::Corrupt ||
											 (Result == EUnrealAISecretStoreResult::Succeeded && Revision == 0)
										 ? EUnrealAIAccountAuthState::ReauthenticationRequired
										 : EUnrealAIAccountAuthState::Failed);
			Recovered.AccessExpiresAtUtc.Reset();
			Recovered.bRefreshCredentialPresent = false;
			bRecoveredSignedOutConfirmed = Result == EUnrealAISecretStoreResult::NotFound;
			bQuarantineRecovered = Result == EUnrealAISecretStoreResult::Corrupt ||
								   (Result == EUnrealAISecretStoreResult::Succeeded && Revision == 0);
		}
		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || SessionEpoch != RecoveryEpoch || !ActiveOperations.IsEmpty())
			{
				return;
			}
			if (Result == EUnrealAISecretStoreResult::NotFound)
			{
				bLocalReauthenticationQuarantine = false;
			}
			else if (bLocalReauthenticationQuarantine)
			{
				Recovered.State = EUnrealAIAccountAuthState::ReauthenticationRequired;
				Recovered.AccessExpiresAtUtc.Reset();
				Recovered.bRefreshCredentialPresent = false;
				bRecoveredSignedOutConfirmed = false;
				bQuarantineRecovered = true;
			}
			else if (bQuarantineRecovered)
			{
				bLocalReauthenticationQuarantine = true;
			}
			Status = Recovered;
			bConfirmedSignedOut = bRecoveredSignedOutConfirmed;
			LocalBroker = Broker.Pin();
		}
		if (LocalBroker.IsValid())
		{
			if (bQuarantineRecovered)
			{
				LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
			}
			else
			{
				LocalBroker->InvalidateAccount(Config.AuthProfileId, Config.AccountId);
			}
		}
	}

	bool TryMakeStoreContext(const FDeviceOAuthAuthOperation &Operation,
							 FUnrealAISecretStoreOperationContext &OutContext) const
	{
		FString ContextError;
		return FUnrealAISecretStoreOperationContext::TryCreate(
			Clock, FMath::Min(Operation.RemainingSeconds(), FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds),
			Operation.GetCancellationToken(), OutContext, ContextError);
	}

	void PublishSignInFailure(const TSharedRef<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe> &Operation,
							  const FUnrealAIProviderAccessError &Error) const
	{
		Operation->PublishFailed(Error);
	}

	void CompensateStaleStore(const uint64 StoredRevision, const FUnrealAISecretValue &AttemptedValue)
	{
		QuarantineBroker();
		if (StoredRevision == 0 || !AttemptedValue.IsSet())
		{
			QuarantineLocalRecovery();
			return;
		}
		// The winning sign-in operation may already be cancelled or timed out. Re-read under a fresh lineage and delete
		// only when both the exact CAS revision and attempted bytes are still present; a later writer always wins.
		FUnrealAICancellationSource CleanupCancellation;
		FUnrealAISecretStoreOperationContext CleanupContext;
		FString ContextError;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(
				Clock, FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds, CleanupCancellation.GetToken(),
				CleanupContext, ContextError))
		{
			QuarantineLocalRecovery();
			return;
		}
		FUnrealAISecretValue Observed;
		uint64 ObservedRevision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult LoadResult =
			SecretStore->Load(CleanupContext, Config.SecretHandle, Observed, ObservedRevision, StoreError);
		const bool bExactAttemptedValue = LoadResult == EUnrealAISecretStoreResult::Succeeded &&
										  ObservedRevision == StoredRevision && SecretsMatch(AttemptedValue, Observed);
		Observed.Reset();
		if (LoadResult == EUnrealAISecretStoreResult::NotFound)
		{
			return;
		}
		if (bExactAttemptedValue)
		{
			const EUnrealAISecretStoreResult DeleteResult =
				SecretStore->Delete(CleanupContext, Config.SecretHandle, StoredRevision, StoreError);
			if (DeleteResult == EUnrealAISecretStoreResult::Succeeded ||
				DeleteResult == EUnrealAISecretStoreResult::NotFound)
			{
				return;
			}
		}
		QuarantineLocalRecovery();
	}

	void ReconcileAmbiguousStore(const uint64 PriorRevision, const FUnrealAISecretValue &AttemptedValue)
	{
		QuarantineBroker();
		if (!AttemptedValue.IsSet() || PriorRevision == TNumericLimits<uint64>::Max())
		{
			QuarantineLocalRecovery();
			return;
		}
		FUnrealAICancellationSource CleanupCancellation;
		FUnrealAISecretStoreOperationContext CleanupContext;
		FString ContextError;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(
				Clock, FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds, CleanupCancellation.GetToken(),
				CleanupContext, ContextError))
		{
			QuarantineLocalRecovery();
			return;
		}
		FUnrealAISecretValue Observed;
		uint64 ObservedRevision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult LoadResult =
			SecretStore->Load(CleanupContext, Config.SecretHandle, Observed, ObservedRevision, StoreError);
		const bool bExactAttemptedValue = LoadResult == EUnrealAISecretStoreResult::Succeeded &&
										  ObservedRevision == PriorRevision + 1 &&
										  SecretsMatch(AttemptedValue, Observed);
		Observed.Reset();
		if (LoadResult == EUnrealAISecretStoreResult::NotFound)
		{
			return;
		}
		if (bExactAttemptedValue)
		{
			const EUnrealAISecretStoreResult DeleteResult =
				SecretStore->Delete(CleanupContext, Config.SecretHandle, ObservedRevision, StoreError);
			if (DeleteResult == EUnrealAISecretStoreResult::Succeeded ||
				DeleteResult == EUnrealAISecretStoreResult::NotFound)
			{
				return;
			}
		}
		QuarantineLocalRecovery();
	}

	static bool SecretsMatch(const FUnrealAISecretValue &A, const FUnrealAISecretValue &B)
	{
		const TConstArrayView<uint8> ABytes = A.View();
		const TConstArrayView<uint8> BBytes = B.View();
		if (ABytes.Num() != BBytes.Num())
		{
			return false;
		}
		uint8 Difference = 0;
		for (int32 Index = 0; Index < ABytes.Num(); ++Index)
		{
			Difference |= ABytes[Index] ^ BBytes[Index];
		}
		return Difference == 0;
	}

	void QuarantineBroker()
	{
		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		{
			FScopeLock Lock(&Mutex);
			LocalBroker = Broker.Pin();
		}
		if (LocalBroker.IsValid())
		{
			LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
		}
	}

	void QuarantineLocalRecovery()
	{
		FScopeLock Lock(&Mutex);
		if (!bShutdown)
		{
			bLocalReauthenticationQuarantine = true;
			bConfirmedSignedOut = false;
		}
	}

	bool IsEpochCurrent(const uint64 OperationEpoch) const
	{
		FScopeLock Lock(&Mutex);
		return !bShutdown && SessionEpoch == OperationEpoch;
	}

	void SetReauthenticationRequiredIfCurrent(const uint64 OperationEpoch)
	{
		FScopeLock Lock(&Mutex);
		if (!bShutdown && SessionEpoch == OperationEpoch)
		{
			Status.State = EUnrealAIAccountAuthState::ReauthenticationRequired;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = false;
			bLocalReauthenticationQuarantine = true;
		}
	}

	void OnOperationTerminal(const FUnrealAIRequestId &RequestId, const EUnrealAIAuthEventKind Kind,
							 const EUnrealAIAccountAuthState TerminalState)
	{
		FScopeLock Lock(&Mutex);
		ActiveOperations.Remove(RequestId.Value);
		if (!bShutdown)
		{
			Status.State = TerminalState;
			if (TerminalState != EUnrealAIAccountAuthState::Ready)
			{
				Status.AccessExpiresAtUtc.Reset();
				Status.bRefreshCredentialPresent = false;
			}
			if (TerminalState != EUnrealAIAccountAuthState::SignedOut || Kind != EUnrealAIAuthEventKind::Succeeded)
			{
				bConfirmedSignedOut = false;
			}
		}
	}

	void OnPhysicalOperationComplete(const FUnrealAIRequestId &RequestId)
	{
		FScopeLock Lock(&Mutex);
		PhysicalOperationIds.Remove(RequestId.Value);
	}

	void OnRecoveryPhysicalComplete()
	{
		FScopeLock Lock(&Mutex);
		bRecoveryInFlight = false;
	}

	FUnrealAIDeviceOAuthAccountProviderConfig Config;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedRef<IUnrealAIDeviceOAuthAuthorizationDriver, ESPMode::ThreadSafe> Driver;
	mutable FCriticalSection Mutex;
	TWeakPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker;
	TMap<FGuid, TSharedPtr<FDeviceOAuthAuthOperation, ESPMode::ThreadSafe>> ActiveOperations;
	TSet<FGuid> PhysicalOperationIds;
	FUnrealAIAccountStatus Status;
	uint64 SessionEpoch = 1;
	bool bRecoveryStarted = false;
	bool bRecoveryInFlight = false;
	bool bConfirmedSignedOut = false;
	bool bLocalReauthenticationQuarantine = false;
	bool bShutdown = false;
};
} // namespace UE::UnrealAI::Private

bool FUnrealAIDeviceOAuthAccountProviderConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FUnrealAIOAuthTokenEnvelopeBinding Binding{ProviderName, AuthProfileId, AccountId};
	FUnrealAIProviderAccessDescriptor Access;
	Access.ModelProviderName = ModelProviderName;
	Access.AccountAuthProviderName = ProviderName;
	Access.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Access.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Access.Availability = EUnrealAIProviderAccessAvailability::SignedOut;
	Access.SupportClassification =
		EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
	FString AccessError;
	if (!Binding.ValidateShape(OutError) || !SecretHandle.ValidateShape(OutError) || !Access.ValidateShape(AccessError))
	{
		if (OutError.IsEmpty())
		{
			OutError = AccessError;
		}
		return false;
	}
	return true;
}

FUnrealAIDeviceOAuthAccountProvider::FUnrealAIDeviceOAuthAccountProvider(
	const FUnrealAIDeviceOAuthAccountProviderConfig &InConfig,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	TSharedRef<IUnrealAIDeviceOAuthAuthorizationDriver, ESPMode::ThreadSafe> InDriver)
	: State(MakeShared<UE::UnrealAI::Private::FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe>(
		  InConfig, MoveTemp(InSecretStore), MoveTemp(InClock), MoveTemp(InDriver)))
{
}

FUnrealAIDeviceOAuthAccountProvider::~FUnrealAIDeviceOAuthAccountProvider()
{
	BeginShutdown();
}

bool FUnrealAIDeviceOAuthAccountProvider::SetCredentialBroker(
	TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InBroker, FString &OutError)
{
	return State->SetCredentialBroker(MoveTemp(InBroker), OutError);
}

void FUnrealAIDeviceOAuthAccountProvider::StartStoredSessionRecovery()
{
	State->StartStoredSessionRecovery();
}

void FUnrealAIDeviceOAuthAccountProvider::BeginShutdown()
{
	State->BeginShutdown();
}

FName FUnrealAIDeviceOAuthAccountProvider::GetProviderName() const
{
	return State->GetConfig().ProviderName;
}

FUnrealAIAccountAuthCapabilities FUnrealAIDeviceOAuthAccountProvider::DescribeCapabilities() const
{
	FUnrealAIAccountAuthCapabilities Capabilities;
	Capabilities.bDeviceCode = true;
	Capabilities.bRefresh = true;
	return Capabilities;
}

FUnrealAIProviderAccessDescriptor FUnrealAIDeviceOAuthAccountProvider::DescribeAccess() const
{
	const FUnrealAIDeviceOAuthAccountProviderConfig &Config = State->GetConfig();
	FUnrealAIProviderAccessDescriptor Descriptor;
	Descriptor.ModelProviderName = Config.ModelProviderName;
	Descriptor.AccountAuthProviderName = Config.ProviderName;
	Descriptor.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Descriptor.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Descriptor.SupportClassification =
		EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
	const FUnrealAIAccountStatus Status = State->GetStatus(Config.AuthProfileId, Config.AccountId);
	if (State->IsShutdown())
	{
		Descriptor.Availability = EUnrealAIProviderAccessAvailability::Unavailable;
	}
	else if (Status.State == EUnrealAIAccountAuthState::Ready)
	{
		Descriptor.Availability = EUnrealAIProviderAccessAvailability::Available;
	}
	else if (Status.State == EUnrealAIAccountAuthState::ReauthenticationRequired)
	{
		Descriptor.Availability = EUnrealAIProviderAccessAvailability::ReauthenticationRequired;
	}
	else if (Status.State == EUnrealAIAccountAuthState::SignedOut)
	{
		Descriptor.Availability = EUnrealAIProviderAccessAvailability::SignedOut;
	}
	else
	{
		Descriptor.Availability = EUnrealAIProviderAccessAvailability::Unavailable;
	}
	return Descriptor;
}

FUnrealAIAccountStatus FUnrealAIDeviceOAuthAccountProvider::GetStatus(const FName AuthProfileId,
																	  const FUnrealAIAccessAccountId &AccountId) const
{
	return State->GetStatus(AuthProfileId, AccountId);
}

bool FUnrealAIDeviceOAuthAccountProvider::StartSignIn(
	const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIInteractiveAuthRequest &Request,
	TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink, const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIProviderAccessError &OutError)
{
	return State->StartSignIn(Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

bool FUnrealAIDeviceOAuthAccountProvider::StartSignOut(
	const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIAccountAuthRequest &Request,
	TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink, const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIProviderAccessError &OutError)
{
	return State->StartSignOut(Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

int32 FUnrealAIDeviceOAuthAccountProvider::GetActiveOperationCount() const
{
	return State->GetActiveOperationCount();
}

int32 FUnrealAIDeviceOAuthAccountProvider::GetPhysicalOperationCount() const
{
	return State->GetPhysicalOperationCount();
}

bool FUnrealAIDeviceOAuthAccountProvider::IsShutdown() const
{
	return State->IsShutdown();
}
