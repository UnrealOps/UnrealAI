// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIBrowserOAuthAccountProvider.h"

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace
{
using namespace UE::UnrealAI::Private;

FUnrealAIProviderAccessError MakeBrowserAccountError(const EUnrealAIErrorCategory Category,
													 const EUnrealAIProviderAccessErrorCode Code,
													 const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

FUnrealAIProviderAccessError NormalizeBrowserAccountFailure(const FUnrealAIProviderAccessError &Candidate)
{
	FString ShapeError;
	if (!Candidate.IsError() || !Candidate.ValidateShape(ShapeError))
	{
		return MakeBrowserAccountError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
	}
	return Candidate;
}

bool IsBrowserAccountStoreAdmissible(const IUnrealAISecretStore &Store)
{
	const FUnrealAISecretStoreCapabilities Capabilities = Store.DescribeCapabilities();
	FString CapabilityError;
	bool bAdmissible = Capabilities.ValidateShape(CapabilityError) && Capabilities.bAvailableInCurrentBuild &&
					   Capabilities.PersistenceClass == EUnrealAISecretStorePersistenceClass::Persistent &&
					   Capabilities.IsProductionProtected() && Capabilities.bAtomicCompareAndSwap;
#if UE_BUILD_SHIPPING
	bAdmissible = bAdmissible && Capabilities.bAvailableInShipping;
#endif
#if UE_BUILD_SHIPPING && UE_SERVER
	bAdmissible = bAdmissible &&
				  Capabilities.ProtectionClass == EUnrealAISecretStoreProtectionClass::ExternalSecretService &&
				  Capabilities.ScopeClass == EUnrealAISecretStoreScopeClass::Service;
#endif
	return bAdmissible;
}
} // namespace

namespace UE::UnrealAI::Private
{
class FBrowserOAuthAuthOperation final : public IUnrealAIAuthOperationHandle,
										 public TSharedFromThis<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe>
{
  public:
	FBrowserOAuthAuthOperation(
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
		PublishTerminal(EUnrealAIAuthEventKind::Cancelled, CancelledState,
						MakeBrowserAccountError(EUnrealAIErrorCategory::Cancelled,
												EUnrealAIProviderAccessErrorCode::AuthCancelled));
	}

	FUnrealAICancellationToken GetCancellationToken() const
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
								MakeBrowserAccountError(EUnrealAIErrorCategory::Timeout,
														EUnrealAIProviderAccessErrorCode::AuthTimedOut, true));
			}
			else
			{
				PublishTerminal(EUnrealAIAuthEventKind::Cancelled, CancelledState,
								MakeBrowserAccountError(EUnrealAIErrorCategory::Cancelled,
														EUnrealAIProviderAccessErrorCode::AuthCancelled));
			}
			return true;
		}
		if (Deadline.IsExpired(*Clock))
		{
			Cancellation.Cancel(EUnrealAICancellationReason::Timeout);
			PublishTerminal(EUnrealAIAuthEventKind::TimedOut, FailureState,
							MakeBrowserAccountError(EUnrealAIErrorCategory::Timeout,
													EUnrealAIProviderAccessErrorCode::AuthTimedOut, true));
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

	bool TryClaimSucceeded()
	{
		return Terminal.TryComplete(EUnrealAITerminalKind::Succeeded);
	}

	void PublishClaimedSucceeded(const EUnrealAIAccountAuthState State)
	{
		PublishClaimedTerminal(EUnrealAIAuthEventKind::Succeeded, State, {});
	}

	bool PublishSucceeded(const EUnrealAIAccountAuthState State)
	{
		return PublishTerminal(EUnrealAIAuthEventKind::Succeeded, State, {});
	}

	bool PublishFailed(const FUnrealAIProviderAccessError &Error)
	{
		const FUnrealAIProviderAccessError Normalized = NormalizeBrowserAccountFailure(Error);
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
			Event.Error =
				MakeBrowserAccountError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
		}
		if (OnTerminal)
		{
			OnTerminal(RequestId, Event.Kind, Event.State);
		}
		TSharedPtr<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> LocalSink;
		{
			FScopeLock Lock(&SinkMutex);
			LocalSink = MoveTemp(Sink);
		}
		if (LocalSink.IsValid())
		{
			LocalSink->EnqueueAuthEvent(MoveTemp(Event));
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
	TFunction<void(const FUnrealAIRequestId &, EUnrealAIAuthEventKind, EUnrealAIAccountAuthState)> OnTerminal;
	FUnrealAITerminalGuard Terminal;
};

class FBrowserOAuthRevocationSink final : public IUnrealAIOAuthAuthorizationCompletionSink
{
  public:
	FBrowserOAuthRevocationSink() : CompletedEvent(FPlatformProcess::GetSynchEventFromPool(true)) {}

	~FBrowserOAuthRevocationSink() override
	{
		FPlatformProcess::ReturnSynchEventToPool(CompletedEvent);
	}

	void CompleteOAuthAuthorization(FUnrealAIOAuthAuthorizationCompletion &&Completion) override
	{
		{
			FScopeLock Lock(&Mutex);
			if (bCompleted)
			{
				return;
			}
			bCompleted = true;
			bSucceeded = Completion.TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::Succeeded;
			Error = Completion.Error;
		}
		CompletedEvent->Trigger();
	}

	bool Wait(const double TimeoutSeconds, FUnrealAIProviderAccessError &OutError)
	{
		const double RealDeadline = FPlatformTime::Seconds() + FMath::Max(TimeoutSeconds, 0.001) + 1.0;
		while (FPlatformTime::Seconds() < RealDeadline)
		{
			if (CompletedEvent->Wait(10))
			{
				break;
			}
		}
		FScopeLock Lock(&Mutex);
		if (!bCompleted)
		{
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Timeout,
											   EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
			return false;
		}
		OutError = Error;
		return bSucceeded;
	}

  private:
	FCriticalSection Mutex;
	FEvent *CompletedEvent = nullptr;
	FUnrealAIProviderAccessError Error;
	bool bCompleted = false;
	bool bSucceeded = false;
};

class FBrowserOAuthAccountProviderState final
	: public TSharedFromThis<FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe>
{
  public:
	FBrowserOAuthAccountProviderState(
		const FUnrealAIBrowserOAuthAccountProviderConfig &InConfig,
		TSharedRef<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe> InTransaction,
		TSharedRef<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe> InAuthorizationCoordinator,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock)
		: Config(InConfig), Transaction(MoveTemp(InTransaction)),
		  AuthorizationCoordinator(MoveTemp(InAuthorizationCoordinator)), SecretStore(MoveTemp(InSecretStore)),
		  Clock(MoveTemp(InClock))
	{
		FString ConfigError;
		bShutdown = !Config.ValidateShape(ConfigError) || !Transaction->IsAvailable() ||
					Config.SecretHandle.StoreName != SecretStore->GetStoreName() ||
					!IsBrowserAccountStoreAdmissible(*SecretStore);
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
			OutError = bShutdown ? TEXT("Browser OAuth account provider is shut down.")
								 : TEXT("Browser OAuth account provider already has a credential broker.");
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
			if (bShutdown || !Broker.IsValid() || bRecoveryStarted || bRecoveryInFlight || SignOutPhysicalCount != 0 ||
				SignInPhysicalCount != 0)
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
		const TSharedRef<FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe> Self = AsShared();
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
		// Signed-out status is intentionally unlinkable. Exact account selection lives in the trusted catalog/request,
		// not in the public signed-out status envelope.
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
		if (!Request.ValidateShape(ShapeError) || Request.Flow != EUnrealAIInteractiveAuthFlow::BrowserPkce ||
			Request.AuthProfileId != Config.AuthProfileId || !Cancellation.IsValid())
		{
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::InvalidArgument,
											   EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Cancelled,
											   EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}
		const EUnrealAIAccountAuthState EffectiveState = GetStatus(Request.AuthProfileId, Config.AccountId).State;
		if (EffectiveState == EUnrealAIAccountAuthState::Ready ||
			EffectiveState == EUnrealAIAccountAuthState::Refreshing ||
			EffectiveState == EUnrealAIAccountAuthState::Revoking ||
			EffectiveState == EUnrealAIAccountAuthState::Authorizing)
		{
			OutError =
				MakeBrowserAccountError(EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::OperationBusy);
			return false;
		}
		const bool bReauthentication = EffectiveState == EUnrealAIAccountAuthState::ReauthenticationRequired;
		const EUnrealAIAccountAuthState CancelledState = bReauthentication
															 ? EUnrealAIAccountAuthState::ReauthenticationRequired
															 : EUnrealAIAccountAuthState::SignedOut;
		const EUnrealAIAccountAuthState FailureState =
			bReauthentication ? EUnrealAIAccountAuthState::ReauthenticationRequired : EUnrealAIAccountAuthState::Failed;
		uint64 OperationEpoch = 0;
		const TWeakPtr<FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe> WeakSelf = AsShared();
		TSharedPtr<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe> Operation;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || !Broker.IsValid() || bRecoveryInFlight || SignOutPhysicalCount != 0 ||
				SignInPhysicalCount != 0 || ActiveOperations.Contains(Request.RequestId.Value) ||
				Status.State == EUnrealAIAccountAuthState::Refreshing ||
				Status.State == EUnrealAIAccountAuthState::Revoking ||
				Status.State == EUnrealAIAccountAuthState::Authorizing ||
				(Status.State == EUnrealAIAccountAuthState::Ready && !bReauthentication))
			{
				OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Busy,
												   EUnrealAIProviderAccessErrorCode::OperationBusy, true);
				return false;
			}
			OperationEpoch = ++SessionEpoch;
			Operation = MakeShared<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe>(
				Request.RequestId, Request.AuthProfileId, Config.AccountId, EUnrealAIAuthOperationKind::SignIn,
				CancelledState, FailureState, Request.TimeoutSeconds, Clock, Sink, Cancellation,
				[WeakSelf, OperationEpoch](const FUnrealAIRequestId &CompletedRequest,
										   const EUnrealAIAuthEventKind Kind, const EUnrealAIAccountAuthState State)
				{
					if (const TSharedPtr<FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe> Pinned =
							WeakSelf.Pin())
					{
						Pinned->OnOperationTerminal(CompletedRequest, OperationEpoch, Kind, State);
					}
				});
			Status.State = EUnrealAIAccountAuthState::Authorizing;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = false;
			ActiveOperations.Add(Request.RequestId.Value, Operation);
			++SignInPhysicalCount;
		}
		OutHandle = Operation;
		const TSharedRef<FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe> Self = AsShared();
		(void)Async(EAsyncExecution::Thread,
					[Self, Operation = Operation.ToSharedRef(), OperationEpoch]()
					{
						Self->RunSignIn(Operation, OperationEpoch);
						Self->OnPhysicalOperationComplete(EUnrealAIAuthOperationKind::SignIn);
					});
		(void)Async(EAsyncExecution::Thread, [Operation = Operation.ToSharedRef()]() { Operation->Monitor(); });
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
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::InvalidArgument,
											   EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Cancelled,
											   EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}
		uint64 OperationEpoch = 0;
		bool bCancelRecovery = false;
		TArray<TSharedPtr<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe>> SignInsToCancel;
		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		const TWeakPtr<FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe> WeakSelf = AsShared();
		TSharedPtr<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe> Operation;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || !Broker.IsValid() || SignOutPhysicalCount != 0 ||
				ActiveOperations.Contains(Request.RequestId.Value))
			{
				OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Busy,
												   EUnrealAIProviderAccessErrorCode::OperationBusy, true);
				return false;
			}
			OperationEpoch = ++SessionEpoch;
			bCancelRecovery = bRecoveryInFlight;
			Operation = MakeShared<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe>(
				Request.RequestId, Request.AuthProfileId, Request.AccountId, EUnrealAIAuthOperationKind::SignOut,
				EUnrealAIAccountAuthState::ReauthenticationRequired,
				EUnrealAIAccountAuthState::ReauthenticationRequired, Request.TimeoutSeconds, Clock, Sink, Cancellation,
				[WeakSelf, OperationEpoch](const FUnrealAIRequestId &CompletedRequest,
										   const EUnrealAIAuthEventKind Kind, const EUnrealAIAccountAuthState State)
				{
					if (const TSharedPtr<FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe> Pinned =
							WeakSelf.Pin())
					{
						Pinned->OnOperationTerminal(CompletedRequest, OperationEpoch, Kind, State);
					}
				});
			for (const TPair<FGuid, TSharedPtr<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe>> &Pair :
				 ActiveOperations)
			{
				if (Pair.Value.IsValid())
				{
					SignInsToCancel.Add(Pair.Value);
				}
			}
			Status.State = EUnrealAIAccountAuthState::Revoking;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = false;
			ActiveOperations.Add(Request.RequestId.Value, Operation);
			++SignOutPhysicalCount;
			LocalBroker = Broker.Pin();
		}

		if (bCancelRecovery)
		{
			RecoveryCancellation.Cancel(EUnrealAICancellationReason::Requested);
		}
		const FUnrealAIOAuthTokenEnvelopeBinding Binding{Config.ProviderName, Config.AuthProfileId, Config.AccountId};
		Transaction->InvalidateAccountForSignOut(Binding);
		for (const TSharedPtr<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe> &SignIn : SignInsToCancel)
		{
			if (SignIn.IsValid() && SignIn != Operation)
			{
				SignIn->Cancel();
			}
		}
		if (LocalBroker.IsValid())
		{
			LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
		}

		OutHandle = Operation;
		const TSharedRef<FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe> Self = AsShared();
		(void)Async(EAsyncExecution::Thread,
					[Self, Operation = Operation.ToSharedRef(), OperationEpoch]()
					{
						Self->RunSignOut(Operation, OperationEpoch);
						Self->OnPhysicalOperationComplete(EUnrealAIAuthOperationKind::SignOut);
					});
		(void)Async(EAsyncExecution::Thread, [Operation = Operation.ToSharedRef()]() { Operation->Monitor(); });
		return true;
	}

	void BeginShutdown()
	{
		TArray<TSharedPtr<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe>> Pending;
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
		RecoveryCancellation.Cancel(EUnrealAICancellationReason::Shutdown);
		const FUnrealAIOAuthTokenEnvelopeBinding Binding{Config.ProviderName, Config.AuthProfileId, Config.AccountId};
		Transaction->InvalidateAccountForSignOut(Binding);
		if (LocalBroker.IsValid())
		{
			LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
		}
		for (const TSharedPtr<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe> &Operation : Pending)
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
		return SignInPhysicalCount + SignOutPhysicalCount + (bRecoveryInFlight ? 1 : 0);
	}

	bool IsShutdown() const
	{
		FScopeLock Lock(&Mutex);
		return bShutdown;
	}

	const FUnrealAIBrowserOAuthAccountProviderConfig &GetConfig() const
	{
		return Config;
	}

  private:
	FUnrealAIOAuthDurableAccountTransactionRequest
	MakeTransactionRequest(const FBrowserOAuthAuthOperation &Operation) const
	{
		FUnrealAIOAuthDurableAccountTransactionRequest Request;
		Request.AuthorizationRequest =
			Config.Authorization.MakeRequest(Operation.GetRequestId(), Operation.RemainingSeconds());
		{
			FScopeLock Lock(&Mutex);
			if (Request.AuthorizationRequest.ExpectedSubjectFingerprint.IsEmpty() &&
				!CommittedSubjectFingerprint.IsEmpty())
			{
				Request.AuthorizationRequest.ExpectedSubjectFingerprint = CommittedSubjectFingerprint;
			}
		}
		Request.Binding = {Config.ProviderName, Config.AuthProfileId, Config.AccountId};
		Request.SecretHandle = Config.SecretHandle;
		Request.CompensationTimeoutSeconds = Config.CleanupTimeoutSeconds;
		Request.bRequireRefreshToken = Config.bRequireRefreshToken;
		Request.bRequireAccountRoutingValue = Config.bRequireAccountRoutingValue;
		return Request;
	}

	void RunSignIn(const TSharedRef<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe> &Operation,
				   const uint64 OperationEpoch)
	{
		if (Operation->ObserveLogicalTerminal() || !IsEpochCurrent(OperationEpoch))
		{
			return;
		}
		FUnrealAIOAuthDurableAccountCommitResult Commit;
		FUnrealAIProviderAccessError TransactionError;
		if (!Transaction->AuthorizeAndCommit(MakeTransactionRequest(*Operation), Operation->GetCancellationToken(),
											 Commit, TransactionError))
		{
			if (!Operation->ObserveLogicalTerminal() && IsEpochCurrent(OperationEpoch))
			{
				Operation->PublishFailed(TransactionError);
			}
			return;
		}

		if (!IsEpochCurrent(OperationEpoch))
		{
			// A newer sign-out epoch fenced this operation and owns the exact committed record after this worker
			// returns.
			Commit.Reset();
			return;
		}
		if (Operation->ObserveLogicalTerminal())
		{
			QuarantineBroker();
			FUnrealAIProviderAccessError CleanupError;
			CleanupStoredAccount(CleanupError);
			Commit.Reset();
			return;
		}

		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		{
			FScopeLock Lock(&Mutex);
			LocalBroker = Broker.Pin();
		}
		FString ReplacementError;
		if (!LocalBroker.IsValid() || !LocalBroker->NotifyCredentialReplaced(
										  Config.AuthProfileId, Config.AccountId, Commit.SecretRevision,
										  FMath::Min(Operation->RemainingSeconds(), Config.CleanupTimeoutSeconds),
										  Operation->GetCancellationToken(), ReplacementError))
		{
			if (LocalBroker.IsValid())
			{
				LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
			}
			if (IsEpochCurrent(OperationEpoch))
			{
				FUnrealAIProviderAccessError CleanupError;
				CleanupStoredAccount(CleanupError);
				if (!Operation->ObserveLogicalTerminal())
				{
					Operation->PublishFailed(MakeBrowserAccountError(
						EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed,
						CleanupError.bRetryable));
				}
			}
			Commit.Reset();
			return;
		}

		if (!IsEpochCurrent(OperationEpoch))
		{
			Commit.Reset();
			return;
		}
		if (Operation->ObserveLogicalTerminal())
		{
			LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
			FUnrealAIProviderAccessError CleanupError;
			CleanupStoredAccount(CleanupError);
			Commit.Reset();
			return;
		}

		bool bSuccessClaimed = false;
		{
			FScopeLock Lock(&Mutex);
			if (!bShutdown && SessionEpoch == OperationEpoch && Operation->TryClaimSucceeded())
			{
				Status.State = EUnrealAIAccountAuthState::Ready;
				Status.AccessExpiresAtUtc = Commit.AccessTokenExpiresAtUtc;
				Status.bRefreshCredentialPresent = Commit.bRefreshCredentialPresent;
				CommittedSubjectFingerprint = Commit.SubjectFingerprint;
				bConfirmedSignedOut = false;
				bLocalReauthenticationQuarantine = false;
				ActiveOperations.Remove(Operation->GetRequestId().Value);
				bSuccessClaimed = true;
			}
		}
		Commit.Reset();
		if (!bSuccessClaimed)
		{
			if (IsEpochCurrent(OperationEpoch))
			{
				LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
				FUnrealAIProviderAccessError CleanupError;
				CleanupStoredAccount(CleanupError);
			}
			return;
		}
		Operation->PublishClaimedSucceeded(EUnrealAIAccountAuthState::Ready);
	}

	void RunSignOut(const TSharedRef<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe> &Operation,
					const uint64 OperationEpoch)
	{
		if (!WaitForMutationSettlement())
		{
			SetReauthenticationRequiredIfCurrent(OperationEpoch);
			if (!Operation->IsComplete())
			{
				Operation->PublishFailed(MakeBrowserAccountError(
					EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true));
			}
			return;
		}
		// Recovery can clear a broker quarantine only after verifying a store revision. Reassert the fence after every
		// earlier account mutation has physically settled, before loading the exact revision to revoke and delete.
		QuarantineBroker();
		FUnrealAIProviderAccessError CleanupError;
		if (!CleanupStoredAccount(CleanupError))
		{
			SetReauthenticationRequiredIfCurrent(OperationEpoch);
			if (!Operation->IsComplete())
			{
				Operation->PublishFailed(CleanupError);
			}
			return;
		}
		TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> LocalBroker;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || SessionEpoch != OperationEpoch)
			{
				return;
			}
			Status.State = EUnrealAIAccountAuthState::SignedOut;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			CommittedSubjectFingerprint.Reset();
			bConfirmedSignedOut = true;
			bLocalReauthenticationQuarantine = false;
			LocalBroker = Broker.Pin();
		}
		if (LocalBroker.IsValid())
		{
			// The record is gone at this point. This final epoch bump closes any lease or resolve that raced the remote
			// revocation and exact delete, while retaining the earlier fail-closed quarantine on unsuccessful cleanup.
			LocalBroker->InvalidateAccount(Config.AuthProfileId, Config.AccountId);
		}
		Operation->PublishSucceeded(EUnrealAIAccountAuthState::SignedOut);
	}

	bool WaitForMutationSettlement() const
	{
		const double RealDeadline = FPlatformTime::Seconds() + Config.CleanupTimeoutSeconds;
		for (;;)
		{
			{
				FScopeLock Lock(&Mutex);
				if (SignInPhysicalCount == 0 && !bRecoveryInFlight)
				{
					return true;
				}
				if (bShutdown)
				{
					return false;
				}
			}
			if (FPlatformTime::Seconds() >= RealDeadline)
			{
				return false;
			}
			FPlatformProcess::SleepNoStats(0.001f);
		}
	}

	bool CleanupStoredAccount(FUnrealAIProviderAccessError &OutError)
	{
		OutError = {};
		FUnrealAICancellationSource StoreCancellation;
		FUnrealAISecretStoreOperationContext LoadContext;
		FString ContextError;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, Config.CleanupTimeoutSeconds,
															 StoreCancellation.GetToken(), LoadContext, ContextError))
		{
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Persistence,
											   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true);
			return false;
		}
		FUnrealAISecretValue Encoded;
		uint64 Revision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult LoadResult =
			SecretStore->Load(LoadContext, Config.SecretHandle, Encoded, Revision, StoreError);
		if (LoadResult == EUnrealAISecretStoreResult::NotFound && Revision == 0 && !Encoded.IsSet())
		{
			return true;
		}
		if (LoadResult != EUnrealAISecretStoreResult::Succeeded || Revision == 0 || !Encoded.IsSet())
		{
			Encoded.Reset();
			OutError = MakeBrowserAccountError(
				EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed,
				StoreError.bRetryable || LoadResult == EUnrealAISecretStoreResult::Locked ||
					LoadResult == EUnrealAISecretStoreResult::Unavailable ||
					LoadResult == EUnrealAISecretStoreResult::TimedOut);
			return false;
		}

		const FUnrealAIOAuthTokenEnvelopeBinding Binding{Config.ProviderName, Config.AuthProfileId, Config.AccountId};
		FUnrealAIOAuthTokenEnvelope Envelope;
		FString EnvelopeError;
		if (!FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeForRefresh(MoveTemp(Encoded), Binding, Clock->UtcNow(),
																   Envelope, EnvelopeError))
		{
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Persistence,
											   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed);
			return false;
		}
		FUnrealAISecretValue RevocationToken;
		if (!Envelope.TryTakeRevocationCredential(RevocationToken, EnvelopeError))
		{
			Envelope.Reset();
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Provider,
											   EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete);
			return false;
		}
		Envelope.Reset();

		FUnrealAIOAuthRevocationRequest RevocationRequest;
		RevocationRequest.RequestId.Value = FGuid::NewGuid();
		RevocationRequest.Server = Config.Authorization.Server;
		RevocationRequest.ClientId.Append(Config.Authorization.ClientId);
		RevocationRequest.TimeoutSeconds = Config.CleanupTimeoutSeconds;
		FUnrealAICancellationSource RevocationCancellation;
		const TSharedRef<FBrowserOAuthRevocationSink, ESPMode::ThreadSafe> RevocationSink =
			MakeShared<FBrowserOAuthRevocationSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> RevocationHandle;
		FUnrealAIProviderAccessError RevocationError;
		if (!AuthorizationCoordinator->StartRevoke(RevocationRequest, MoveTemp(RevocationToken), RevocationSink,
												   RevocationCancellation.GetToken(), RevocationHandle,
												   RevocationError) ||
			!RevocationSink->Wait(Config.CleanupTimeoutSeconds, RevocationError))
		{
			if (RevocationHandle.IsValid())
			{
				RevocationHandle->Cancel();
			}
			OutError = NormalizeBrowserAccountFailure(RevocationError);
			if (!OutError.IsError())
			{
				OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Provider,
												   EUnrealAIProviderAccessErrorCode::AuthFailed, true);
			}
			return false;
		}

		FUnrealAICancellationSource DeleteCancellation;
		FUnrealAISecretStoreOperationContext DeleteContext;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(
				Clock, Config.CleanupTimeoutSeconds, DeleteCancellation.GetToken(), DeleteContext, ContextError))
		{
			OutError = MakeBrowserAccountError(EUnrealAIErrorCategory::Persistence,
											   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true);
			return false;
		}
		const EUnrealAISecretStoreResult DeleteResult =
			SecretStore->Delete(DeleteContext, Config.SecretHandle, Revision, StoreError);
		if (DeleteResult != EUnrealAISecretStoreResult::Succeeded &&
			DeleteResult != EUnrealAISecretStoreResult::NotFound)
		{
			OutError = MakeBrowserAccountError(
				EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed,
				StoreError.bRetryable || DeleteResult == EUnrealAISecretStoreResult::Locked ||
					DeleteResult == EUnrealAISecretStoreResult::Unavailable ||
					DeleteResult == EUnrealAISecretStoreResult::TimedOut ||
					DeleteResult == EUnrealAISecretStoreResult::Conflict);
			return false;
		}
		return true;
	}

	void RecoverStoredSession(const uint64 RecoveryEpoch)
	{
		FUnrealAISecretStoreOperationContext StoreContext;
		FString ContextError;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(
				Clock, Config.CleanupTimeoutSeconds, RecoveryCancellation.GetToken(), StoreContext, ContextError))
		{
			SetRecoveryFailureIfCurrent(RecoveryEpoch, EUnrealAIAccountAuthState::Failed, false);
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
				(!Config.bRequireRefreshToken || Envelope.HasRefreshToken()) &&
				(!Config.bRequireAccountRoutingValue || Envelope.HasAccountRoutingValue()))
			{
				Recovered.State = EUnrealAIAccountAuthState::Ready;
				Recovered.AccessExpiresAtUtc = Envelope.GetAccessTokenExpiresAtUtc();
				Recovered.bRefreshCredentialPresent = Envelope.HasRefreshToken();
			}
			else
			{
				Recovered.State = EUnrealAIAccountAuthState::ReauthenticationRequired;
				bQuarantineRecovered = true;
			}
			Envelope.Reset();
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
			LocalBroker = Broker.Pin();
		}
		if (Recovered.State == EUnrealAIAccountAuthState::Ready)
		{
			FString ReplacementError;
			if (!LocalBroker.IsValid() ||
				!LocalBroker->NotifyCredentialReplaced(Config.AuthProfileId, Config.AccountId, Revision,
													   Config.CleanupTimeoutSeconds, RecoveryCancellation.GetToken(),
													   ReplacementError))
			{
				Recovered.State = EUnrealAIAccountAuthState::ReauthenticationRequired;
				Recovered.AccessExpiresAtUtc.Reset();
				Recovered.bRefreshCredentialPresent = false;
				bQuarantineRecovered = true;
			}
		}

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
		}
		if (LocalBroker.IsValid())
		{
			if (bQuarantineRecovered)
			{
				LocalBroker->QuarantineAccountForReauthentication(Config.AuthProfileId, Config.AccountId);
			}
			else if (Recovered.State != EUnrealAIAccountAuthState::Ready)
			{
				LocalBroker->InvalidateAccount(Config.AuthProfileId, Config.AccountId);
			}
		}
	}

	void SetRecoveryFailureIfCurrent(const uint64 RecoveryEpoch, const EUnrealAIAccountAuthState State,
									 const bool bQuarantine)
	{
		FScopeLock Lock(&Mutex);
		if (!bShutdown && SessionEpoch == RecoveryEpoch)
		{
			Status.State = State;
			Status.AccessExpiresAtUtc.Reset();
			Status.bRefreshCredentialPresent = false;
			bConfirmedSignedOut = false;
			bLocalReauthenticationQuarantine = bQuarantine;
		}
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

	void OnOperationTerminal(const FUnrealAIRequestId &RequestId, const uint64 OperationEpoch,
							 const EUnrealAIAuthEventKind Kind, const EUnrealAIAccountAuthState TerminalState)
	{
		FScopeLock Lock(&Mutex);
		ActiveOperations.Remove(RequestId.Value);
		if (!bShutdown && SessionEpoch == OperationEpoch)
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

	void OnPhysicalOperationComplete(const EUnrealAIAuthOperationKind Kind)
	{
		FScopeLock Lock(&Mutex);
		if (Kind == EUnrealAIAuthOperationKind::SignIn)
		{
			SignInPhysicalCount = FMath::Max(0, SignInPhysicalCount - 1);
		}
		else if (Kind == EUnrealAIAuthOperationKind::SignOut)
		{
			SignOutPhysicalCount = FMath::Max(0, SignOutPhysicalCount - 1);
		}
	}

	void OnRecoveryPhysicalComplete()
	{
		FScopeLock Lock(&Mutex);
		bRecoveryInFlight = false;
	}

	FUnrealAIBrowserOAuthAccountProviderConfig Config;
	TSharedRef<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe> Transaction;
	TSharedRef<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe> AuthorizationCoordinator;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	mutable FCriticalSection Mutex;
	TWeakPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> Broker;
	TMap<FGuid, TSharedPtr<FBrowserOAuthAuthOperation, ESPMode::ThreadSafe>> ActiveOperations;
	FUnrealAICancellationSource RecoveryCancellation;
	FUnrealAIAccountStatus Status;
	FString CommittedSubjectFingerprint;
	uint64 SessionEpoch = 1;
	int32 SignInPhysicalCount = 0;
	int32 SignOutPhysicalCount = 0;
	bool bRecoveryStarted = false;
	bool bRecoveryInFlight = false;
	bool bConfirmedSignedOut = false;
	bool bLocalReauthenticationQuarantine = false;
	bool bShutdown = false;
};
} // namespace UE::UnrealAI::Private

bool FUnrealAIBrowserOAuthAuthorizationPolicy::ValidateShape(FString &OutError) const
{
	const FUnrealAIRequestId ValidationRequestId{FGuid(0x0a0b0c0d, 0x1a1b1c1d, 0x2a2b2c2d, 0x3a3b3c3d)};
	return MakeRequest(ValidationRequestId, 30.0).ValidateShape(OutError);
}

FUnrealAIOAuthBrowserAuthorizationRequest
FUnrealAIBrowserOAuthAuthorizationPolicy::MakeRequest(const FUnrealAIRequestId &RequestId,
													  const double TimeoutSeconds) const
{
	FUnrealAIOAuthBrowserAuthorizationRequest Request;
	Request.RequestId = RequestId;
	Request.Server = Server;
	Request.ClientId.Append(ClientId);
	Request.Audience = Audience;
	Request.ExactRedirectUri = ExactRedirectUri;
	Request.RequestedScopes = RequestedScopes;
	Request.ExpectedSubjectFingerprint = ExpectedSubjectFingerprint;
	Request.TimeoutSeconds = TimeoutSeconds;
	Request.ClockSkewSeconds = ClockSkewSeconds;
	Request.MaxIdentityTokenAgeSeconds = MaxIdentityTokenAgeSeconds;
	Request.MaxJwksCacheAgeSeconds = MaxJwksCacheAgeSeconds;
	return Request;
}

bool FUnrealAIBrowserOAuthAccountProviderConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString NestedError;
	const FUnrealAIOAuthTokenEnvelopeBinding Binding{ProviderName, AuthProfileId, AccountId};
	FUnrealAIProviderAccessDescriptor Access;
	Access.ModelProviderName = ModelProviderName;
	Access.AccountAuthProviderName = ProviderName;
	Access.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Access.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Access.Availability = EUnrealAIProviderAccessAvailability::SignedOut;
	Access.SupportClassification = SupportClassification;
	if (!Binding.ValidateShape(NestedError) || !SecretHandle.ValidateShape(NestedError) ||
		!Authorization.ValidateShape(NestedError) || Authorization.Server.RevocationEndpoint.IsEmpty() ||
		!Access.ValidateShape(NestedError) || !FMath::IsFinite(CleanupTimeoutSeconds) || CleanupTimeoutSeconds <= 0.0 ||
		CleanupTimeoutSeconds > FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds)
	{
		OutError = TEXT("Browser OAuth account-provider configuration is invalid.");
		return false;
	}
	return true;
}

FUnrealAIBrowserOAuthAccountProvider::FUnrealAIBrowserOAuthAccountProvider(
	const FUnrealAIBrowserOAuthAccountProviderConfig &InConfig,
	TSharedRef<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe> InTransaction,
	TSharedRef<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe> InAuthorizationCoordinator,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock)
	: State(MakeShared<UE::UnrealAI::Private::FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe>(
		  InConfig, MoveTemp(InTransaction), MoveTemp(InAuthorizationCoordinator), MoveTemp(InSecretStore),
		  MoveTemp(InClock)))
{
}

FUnrealAIBrowserOAuthAccountProvider::~FUnrealAIBrowserOAuthAccountProvider()
{
	BeginShutdown();
}

bool FUnrealAIBrowserOAuthAccountProvider::SetCredentialBroker(
	TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InBroker, FString &OutError)
{
	return State->SetCredentialBroker(MoveTemp(InBroker), OutError);
}

void FUnrealAIBrowserOAuthAccountProvider::StartStoredSessionRecovery()
{
	State->StartStoredSessionRecovery();
}

void FUnrealAIBrowserOAuthAccountProvider::BeginShutdown()
{
	State->BeginShutdown();
}

FName FUnrealAIBrowserOAuthAccountProvider::GetProviderName() const
{
	return State->GetConfig().ProviderName;
}

FUnrealAIAccountAuthCapabilities FUnrealAIBrowserOAuthAccountProvider::DescribeCapabilities() const
{
	FUnrealAIAccountAuthCapabilities Capabilities;
	Capabilities.bBrowserPkce = true;
	Capabilities.bRefresh = State->GetConfig().bRequireRefreshToken;
	Capabilities.bRevocation = true;
	return Capabilities;
}

FUnrealAIProviderAccessDescriptor FUnrealAIBrowserOAuthAccountProvider::DescribeAccess() const
{
	const FUnrealAIBrowserOAuthAccountProviderConfig &Config = State->GetConfig();
	FUnrealAIProviderAccessDescriptor Descriptor;
	Descriptor.ModelProviderName = Config.ModelProviderName;
	Descriptor.AccountAuthProviderName = Config.ProviderName;
	Descriptor.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Descriptor.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Descriptor.SupportClassification = Config.SupportClassification;
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

FUnrealAIAccountStatus FUnrealAIBrowserOAuthAccountProvider::GetStatus(const FName AuthProfileId,
																	   const FUnrealAIAccessAccountId &AccountId) const
{
	return State->GetStatus(AuthProfileId, AccountId);
}

bool FUnrealAIBrowserOAuthAccountProvider::StartSignIn(
	const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIInteractiveAuthRequest &Request,
	TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink, const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIProviderAccessError &OutError)
{
	return State->StartSignIn(Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

bool FUnrealAIBrowserOAuthAccountProvider::StartSignOut(
	const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIAccountAuthRequest &Request,
	TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink, const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle, FUnrealAIProviderAccessError &OutError)
{
	return State->StartSignOut(Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

int32 FUnrealAIBrowserOAuthAccountProvider::GetActiveOperationCount() const
{
	return State->GetActiveOperationCount();
}

int32 FUnrealAIBrowserOAuthAccountProvider::GetPhysicalOperationCount() const
{
	return State->GetPhysicalOperationCount();
}

bool FUnrealAIBrowserOAuthAccountProvider::IsShutdown() const
{
	return State->IsShutdown();
}
