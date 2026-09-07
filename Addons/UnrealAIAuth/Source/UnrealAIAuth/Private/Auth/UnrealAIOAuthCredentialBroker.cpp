// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthCredentialBroker.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

namespace
{
using namespace UE::UnrealAI::Private;

constexpr uint32 RefreshWaitMilliseconds = 10;

FUnrealAIProviderAccessError MakeOAuthBrokerAccessError(const EUnrealAIErrorCategory Category,
														const EUnrealAIProviderAccessErrorCode Code,
														const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

bool IsQuarantiningRefreshError(const FUnrealAIProviderAccessError &Error)
{
	return Error.IsError() && !Error.bRetryable &&
		   (Error.Code == EUnrealAIProviderAccessErrorCode::AccessProfileNotReady ||
			Error.Code == EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied ||
			Error.Code == EUnrealAIProviderAccessErrorCode::CredentialFailed);
}

FUnrealAICredentialResult MakeOAuthBrokerTerminalResult(const FUnrealAIRequestId RequestId,
														const EUnrealAICredentialResultKind Kind,
														const FUnrealAIProviderAccessError &Error = {})
{
	FUnrealAICredentialResult Result;
	Result.RequestId = RequestId;
	Result.Kind = Kind;
	Result.Error = Error;
	if (Kind == EUnrealAICredentialResultKind::Cancelled)
	{
		Result.Error = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Cancelled,
												  EUnrealAIProviderAccessErrorCode::CredentialCancelled);
	}
	else if (Kind == EUnrealAICredentialResultKind::TimedOut)
	{
		Result.Error = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Timeout,
												  EUnrealAIProviderAccessErrorCode::CredentialTimedOut);
	}
	else if (Kind == EUnrealAICredentialResultKind::Failed && !Result.Error.IsError())
	{
		Result.Error = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
												  EUnrealAIProviderAccessErrorCode::CredentialFailed);
	}
	return Result;
}

FUnrealAICredentialResult MakeOAuthBrokerStoreFailure(const FUnrealAIRequestId RequestId,
													  const EUnrealAISecretStoreResult StoreResult,
													  const FUnrealAIProviderAccessError &StoreError)
{
	switch (StoreResult)
	{
	case EUnrealAISecretStoreResult::NotFound:
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::NotFound,
									   EUnrealAIProviderAccessErrorCode::SecretNotFound));
	case EUnrealAISecretStoreResult::Conflict:
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::VersionMismatch,
									   EUnrealAIProviderAccessErrorCode::SecretRevisionConflict, true));
	case EUnrealAISecretStoreResult::Locked:
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Busy,
									   EUnrealAIProviderAccessErrorCode::SecretStoreLocked, true));
	case EUnrealAISecretStoreResult::Denied:
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::PolicyDenied,
									   EUnrealAIProviderAccessErrorCode::SecretStoreDenied));
	case EUnrealAISecretStoreResult::Unavailable:
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Persistence,
									   EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, true));
	case EUnrealAISecretStoreResult::NotSupported:
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::UnsupportedCapability,
									   EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported));
	case EUnrealAISecretStoreResult::Corrupt:
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Persistence,
									   EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt));
	case EUnrealAISecretStoreResult::Cancelled:
		return MakeOAuthBrokerTerminalResult(RequestId, EUnrealAICredentialResultKind::Cancelled);
	case EUnrealAISecretStoreResult::TimedOut:
		return MakeOAuthBrokerTerminalResult(RequestId, EUnrealAICredentialResultKind::TimedOut);
	case EUnrealAISecretStoreResult::Failed:
	{
		FString ShapeError;
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			StoreError.IsError() && StoreError.ValidateShape(ShapeError)
				? StoreError
				: MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Internal,
											 EUnrealAIProviderAccessErrorCode::Internal));
	}
	case EUnrealAISecretStoreResult::Succeeded:
	default:
		return MakeOAuthBrokerTerminalResult(
			RequestId, EUnrealAICredentialResultKind::Failed,
			MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Persistence,
									   EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt));
	}
}

EUnrealAITerminalKind OAuthBrokerTerminalKindFor(const EUnrealAICredentialResultKind Kind)
{
	if (Kind == EUnrealAICredentialResultKind::Succeeded || Kind == EUnrealAICredentialResultKind::NotRequired)
	{
		return EUnrealAITerminalKind::Succeeded;
	}
	if (Kind == EUnrealAICredentialResultKind::Cancelled)
	{
		return EUnrealAITerminalKind::Cancelled;
	}
	if (Kind == EUnrealAICredentialResultKind::TimedOut)
	{
		return EUnrealAITerminalKind::TimedOut;
	}
	return EUnrealAITerminalKind::Failed;
}

struct FOAuthBindingKey final
{
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;

	friend bool operator==(const FOAuthBindingKey &A, const FOAuthBindingKey &B)
	{
		return A.AuthProfileId == B.AuthProfileId && A.AccountId == B.AccountId;
	}
	friend uint32 GetTypeHash(const FOAuthBindingKey &Key)
	{
		return HashCombine(GetTypeHash(Key.AuthProfileId), GetTypeHash(Key.AccountId));
	}
};

struct FOAuthBindingSnapshot final
{
	FUnrealAIOAuthCredentialBinding Binding;
	TSharedPtr<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> Freshness;
	uint64 BindingEpoch = 0;
	uint64 ConnectionEpoch = 0;
	bool bForceRefresh = false;
};
} // namespace

namespace UE::UnrealAI::Private
{
class FOAuthRefreshFlight final
{
  public:
	FOAuthRefreshFlight()
	{
		Event = FPlatformProcess::GetSynchEventFromPool(true);
	}

	~FOAuthRefreshFlight()
	{
		if (Event != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(Event);
			Event = nullptr;
		}
	}

	void Complete(const bool bInSucceeded, const FUnrealAIProviderAccessError &InError)
	{
		{
			FScopeLock Lock(&Mutex);
			if (bComplete)
			{
				return;
			}
			bComplete = true;
			bSucceeded = bInSucceeded;
			Error = InError;
		}
		Event->Trigger();
	}

	bool WaitSlice() const
	{
		return Event->Wait(RefreshWaitMilliseconds);
	}

	bool GetResult(bool &OutSucceeded, FUnrealAIProviderAccessError &OutError) const
	{
		FScopeLock Lock(&Mutex);
		if (!bComplete)
		{
			return false;
		}
		OutSucceeded = bSucceeded;
		OutError = Error;
		return true;
	}

  private:
	mutable FCriticalSection Mutex;
	FEvent *Event = nullptr;
	bool bComplete = false;
	bool bSucceeded = false;
	FUnrealAIProviderAccessError Error;
};

class FOAuthCredentialOperation final : public IUnrealAICredentialRequestHandle,
										public TSharedFromThis<FOAuthCredentialOperation, ESPMode::ThreadSafe>
{
  public:
	FOAuthCredentialOperation(const FUnrealAICredentialRequest &Request,
							  TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> InConnection,
							  const FOAuthBindingSnapshot &InSnapshot,
							  TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
							  TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> InSink,
							  const FUnrealAICancellationToken &ParentCancellation,
							  TFunction<void(const FUnrealAIRequestId &)> InOnTerminal)
		: RequestId(Request.RequestId), Connection(MoveTemp(InConnection)), Snapshot(InSnapshot),
		  Clock(MoveTemp(InClock)), Cancellation(ParentCancellation),
		  Deadline(FUnrealAIDeadline::FromNow(*Clock, Request.TimeoutSeconds)), Sink(InSink.ToSharedPtr()),
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
		Publish(MakeOAuthBrokerTerminalResult(RequestId, EUnrealAICredentialResultKind::Cancelled));
	}

	const FUnrealAIConnectionDescriptor &GetConnection() const
	{
		return Connection.Get();
	}

	const FOAuthBindingSnapshot &GetSnapshot() const
	{
		return Snapshot;
	}

	const FUnrealAICancellationToken GetCancellationToken() const
	{
		return Cancellation.GetToken();
	}

	double RemainingSeconds() const
	{
		return Deadline.RemainingSeconds(*Clock);
	}

	const IUnrealAIClock &GetClock() const
	{
		return *Clock;
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
				Publish(MakeOAuthBrokerTerminalResult(RequestId, EUnrealAICredentialResultKind::TimedOut));
			}
			else
			{
				Publish(MakeOAuthBrokerTerminalResult(RequestId, EUnrealAICredentialResultKind::Cancelled));
			}
			return true;
		}
		if (Deadline.IsExpired(*Clock))
		{
			Cancellation.Cancel(EUnrealAICancellationReason::Timeout);
			Publish(MakeOAuthBrokerTerminalResult(RequestId, EUnrealAICredentialResultKind::TimedOut));
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

	bool Publish(FUnrealAICredentialResult &&Result)
	{
		const EUnrealAITerminalKind TerminalKind = OAuthBrokerTerminalKindFor(Result.Kind);
		if (!Terminal.TryComplete(TerminalKind))
		{
			return false;
		}
		FString ShapeError;
		if (!Result.ValidateShape(ShapeError))
		{
			Result =
				MakeOAuthBrokerTerminalResult(RequestId, EUnrealAICredentialResultKind::Failed,
											  MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Internal,
																		 EUnrealAIProviderAccessErrorCode::Internal));
		}
		TSharedPtr<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> LocalSink;
		{
			FScopeLock Lock(&SinkMutex);
			LocalSink = MoveTemp(Sink);
		}
		if (OnTerminal)
		{
			OnTerminal(RequestId);
		}
		if (LocalSink.IsValid())
		{
			LocalSink->EnqueueCredentialResult(MoveTemp(Result));
		}
		return true;
	}

  private:
	FUnrealAIRequestId RequestId;
	TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection;
	FOAuthBindingSnapshot Snapshot;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAICancellationSource Cancellation;
	FUnrealAIDeadline Deadline;
	mutable FCriticalSection SinkMutex;
	TSharedPtr<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink;
	TFunction<void(const FUnrealAIRequestId &)> OnTerminal;
	FUnrealAITerminalGuard Terminal;
};

class FOAuthCredentialBrokerState final : public TSharedFromThis<FOAuthCredentialBrokerState, ESPMode::ThreadSafe>
{
  public:
	struct FBindingEntry final
	{
		FUnrealAIOAuthCredentialBinding Binding;
		TMap<FName, TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> ConnectionFreshness;
		TOptional<FUnrealAIProviderAccessError> QuarantineError;
		uint64 Epoch = 1;
		bool bForceRefresh = false;
	};

	FOAuthCredentialBrokerState(
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		TSharedRef<IUnrealAIOAuthCredentialRefreshSource, ESPMode::ThreadSafe> InRefreshSource,
		const FUnrealAIOAuthCredentialBrokerConfig &InConfig)
		: Connections(MoveTemp(InConnections)), SecretStore(MoveTemp(InSecretStore)), Clock(MoveTemp(InClock)),
		  RefreshSource(MoveTemp(InRefreshSource)), Config(InConfig)
	{
		FString ConfigError;
		bShutdown = !Config.ValidateShape(ConfigError);
	}

	bool RegisterBinding(const FUnrealAIOAuthCredentialBinding &Binding, FString &OutError)
	{
		OutError.Reset();
		if (!Binding.ValidateShape(OutError) || Binding.ProviderName != RefreshSource->GetProviderName() ||
			Binding.SecretHandle.StoreName != SecretStore->GetStoreName())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("OAuth binding does not match its refresh provider or secure store.");
			}
			return false;
		}
		const FOAuthBindingKey Key{Binding.AuthProfileId, Binding.AccountId};
		FScopeLock Lock(&Mutex);
		if (bShutdown || Bindings.Contains(Key))
		{
			OutError = bShutdown ? TEXT("OAuth broker is shut down.")
								 : TEXT("OAuth broker rejected a duplicate profile/account binding.");
			return false;
		}
		FBindingEntry Entry;
		Entry.Binding = Binding;
		Bindings.Add(Key, MoveTemp(Entry));
		return true;
	}

	bool StartResolve(const FUnrealAICredentialRequest &Request,
					  TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError)
	{
		OutHandle.Reset();
		OutError = {};
		FString ShapeError;
		if (!Request.ValidateShape(ShapeError) || !Cancellation.IsValid())
		{
			OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::InvalidArgument,
												  EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Cancelled,
												  EUnrealAIProviderAccessErrorCode::CredentialCancelled);
			return false;
		}
		const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection =
			Connections->Find(Request.ConnectionAlias);
		if (!Connection.IsValid() || Connection->CredentialDestination.AuthScheme != EUnrealAIAuthScheme::OAuthBearer ||
			Connection->CredentialDestination.BillingMode != EUnrealAIBillingMode::SubscriptionQuota)
		{
			OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::InvalidConfiguration,
												  EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
			return false;
		}
		const FOAuthBindingKey Key{Connection->CredentialDestination.AuthProfileId,
								   Connection->CredentialDestination.AccountId};
		FOAuthBindingSnapshot Snapshot;
		{
			FScopeLock Lock(&Mutex);
			FBindingEntry *Entry = Bindings.Find(Key);
			if (bShutdown)
			{
				OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
													  EUnrealAIProviderAccessErrorCode::CredentialFailed);
				return false;
			}
			if (Entry == nullptr ||
				Entry->Binding.ProviderName != Connection->CredentialDestination.AccountAuthProviderName)
			{
				OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::NotAuthorized,
													  EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
				return false;
			}
			if (Entry->QuarantineError.IsSet())
			{
				OutError = Entry->QuarantineError.GetValue();
				return false;
			}
			if (PhysicalOperationIds.Num() >= Config.MaxConcurrentOperations ||
				PhysicalOperationIds.Contains(Request.RequestId.Value))
			{
				OutError = MakeOAuthBrokerAccessError(
					EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity, true);
				return false;
			}
			TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> *ConnectionFreshness =
				Entry->ConnectionFreshness.Find(Request.ConnectionAlias);
			if (ConnectionFreshness == nullptr)
			{
				ConnectionFreshness = &Entry->ConnectionFreshness.Add(
					Request.ConnectionAlias, MakeShared<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>());
			}
			Snapshot.Binding = Entry->Binding;
			Snapshot.Freshness = ConnectionFreshness->ToSharedPtr();
			Snapshot.BindingEpoch = Entry->Epoch;
			Snapshot.ConnectionEpoch = ConnectionEpochs.FindRef(Request.ConnectionAlias);
			Snapshot.bForceRefresh = Entry->bForceRefresh;
		}

		const TWeakPtr<FOAuthCredentialBrokerState, ESPMode::ThreadSafe> WeakSelf = AsShared();
		const TSharedRef<FOAuthCredentialOperation, ESPMode::ThreadSafe> Operation =
			MakeShared<FOAuthCredentialOperation, ESPMode::ThreadSafe>(
				Request, Connection.ToSharedRef(), Snapshot, Clock, Sink, Cancellation,
				[WeakSelf](const FUnrealAIRequestId &CompletedRequest)
				{
					if (const TSharedPtr<FOAuthCredentialBrokerState, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin())
					{
						Pinned->RemoveOperation(CompletedRequest);
					}
				});
		{
			FScopeLock Lock(&Mutex);
			FBindingEntry *Entry = Bindings.Find(Key);
			if (bShutdown)
			{
				OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
													  EUnrealAIProviderAccessErrorCode::CredentialFailed);
				return false;
			}
			if (Entry == nullptr ||
				Entry->Binding.ProviderName != Connection->CredentialDestination.AccountAuthProviderName)
			{
				OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::NotAuthorized,
													  EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
				return false;
			}
			if (Entry->QuarantineError.IsSet())
			{
				OutError = Entry->QuarantineError.GetValue();
				return false;
			}
			const TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> *CurrentFreshness =
				Entry->ConnectionFreshness.Find(Request.ConnectionAlias);
			if (Entry->Epoch != Snapshot.BindingEpoch || CurrentFreshness == nullptr ||
				CurrentFreshness->ToSharedPtr() != Snapshot.Freshness ||
				ConnectionEpochs.FindRef(Request.ConnectionAlias) != Snapshot.ConnectionEpoch ||
				Entry->bForceRefresh != Snapshot.bForceRefresh || !Snapshot.Freshness.IsValid() ||
				!Snapshot.Freshness->GetToken().IsCurrent())
			{
				OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::NotAuthorized,
													  EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
				return false;
			}
			if (PhysicalOperationIds.Num() >= Config.MaxConcurrentOperations ||
				PhysicalOperationIds.Contains(Request.RequestId.Value))
			{
				OutError = MakeOAuthBrokerAccessError(
					EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity, true);
				return false;
			}
			ActiveOperations.Add(Request.RequestId.Value, Operation);
			PhysicalOperationIds.Add(Request.RequestId.Value);
		}
		OutHandle = Operation;
		const TSharedRef<FOAuthCredentialBrokerState, ESPMode::ThreadSafe> Self = AsShared();
		(void)Async(EAsyncExecution::Thread,
					[Self, Operation]()
					{
						Self->Run(Operation);
						Self->RemovePhysicalOperation(Operation->GetRequestId());
					});
		(void)Async(EAsyncExecution::Thread, [Operation]() { Operation->Monitor(); });
		return true;
	}

	void Run(const TSharedRef<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation)
	{
		if (Operation->ObserveLogicalTerminal() || !IsSnapshotCurrent(*Operation))
		{
			PublishInvalidated(Operation);
			return;
		}
		FUnrealAISecretStoreOperationContext StoreContext;
		FString ContextError;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(
				Clock,
				FMath::Min(Operation->RemainingSeconds(), FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds),
				Operation->GetCancellationToken(), StoreContext, ContextError))
		{
			Operation->Publish(MakeOAuthBrokerTerminalResult(
				Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed,
				MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::InvalidArgument,
										   EUnrealAIProviderAccessErrorCode::InvalidSecretStoreContext)));
			return;
		}

		FUnrealAISecretValue EncodedEnvelope;
		uint64 Revision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult LoadResult = SecretStore->Load(
			StoreContext, Operation->GetSnapshot().Binding.SecretHandle, EncodedEnvelope, Revision, StoreError);
		if (Operation->ObserveLogicalTerminal())
		{
			EncodedEnvelope.Reset();
			return;
		}
		if (LoadResult != EUnrealAISecretStoreResult::Succeeded || Revision == 0)
		{
			FUnrealAICredentialResult Failure =
				MakeOAuthBrokerStoreFailure(Operation->GetRequestId(), LoadResult, StoreError);
			if (LoadResult == EUnrealAISecretStoreResult::NotFound ||
				LoadResult == EUnrealAISecretStoreResult::Corrupt ||
				(LoadResult == EUnrealAISecretStoreResult::Succeeded && Revision == 0))
			{
				QuarantineSnapshot(Operation->GetSnapshot(), Failure.Error);
			}
			Operation->Publish(MoveTemp(Failure));
			return;
		}
		if (!IsSnapshotCurrent(*Operation))
		{
			EncodedEnvelope.Reset();
			PublishInvalidated(Operation);
			return;
		}

		FUnrealAIOAuthTokenEnvelope Envelope;
		FString EnvelopeError;
		const FUnrealAIOAuthTokenEnvelopeBinding ExpectedBinding{Operation->GetSnapshot().Binding.ProviderName,
																 Operation->GetSnapshot().Binding.AuthProfileId,
																 Operation->GetSnapshot().Binding.AccountId};
		if (!FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeForRefresh(
				MoveTemp(EncodedEnvelope), ExpectedBinding, Operation->GetClock().UtcNow(), Envelope, EnvelopeError))
		{
			const FUnrealAIProviderAccessError Error = MakeOAuthBrokerAccessError(
				EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
			QuarantineSnapshot(Operation->GetSnapshot(), Error);
			Operation->Publish(
				MakeOAuthBrokerTerminalResult(Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed, Error));
			return;
		}
		if (!Envelope.HasRefreshToken() ||
			(Operation->GetSnapshot().Binding.bRequiresProtectedSecondary && !Envelope.HasAccountRoutingValue()))
		{
			const FUnrealAIProviderAccessError Error = MakeOAuthBrokerAccessError(
				EUnrealAIErrorCategory::NotAuthorized, EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
			Envelope.Reset();
			QuarantineSnapshot(Operation->GetSnapshot(), Error);
			Operation->Publish(
				MakeOAuthBrokerTerminalResult(Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed, Error));
			return;
		}

		const bool bNeedsRefresh =
			Operation->GetSnapshot().bForceRefresh ||
			Envelope.GetAccessTokenExpiresAtUtc() <=
				Operation->GetClock().UtcNow() + FTimespan::FromSeconds(Config.RefreshSkewSeconds);
		if (bNeedsRefresh)
		{
			if (!RefreshEnvelope(Operation, StoreContext, Revision, Envelope))
			{
				return;
			}
		}
		PublishLease(Operation, MoveTemp(Envelope));
	}

	void InvalidateAccount(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId)
	{
		TArray<TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> OperationsToCancel;
		{
			FScopeLock Lock(&Mutex);
			const FOAuthBindingKey Key{AuthProfileId, AccountId};
			FBindingEntry *Entry = Bindings.Find(Key);
			if (Entry != nullptr)
			{
				for (TPair<FName, TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> &Pair :
					 Entry->ConnectionFreshness)
				{
					Pair.Value->Invalidate();
				}
				Entry->ConnectionFreshness.Reset();
				++Entry->Epoch;
				Entry->bForceRefresh = false;
			}
			CollectMatchingOperationsLocked(
				[&Key](const FOAuthCredentialOperation &Operation)
				{
					return Operation.GetSnapshot().Binding.AuthProfileId == Key.AuthProfileId &&
						   Operation.GetSnapshot().Binding.AccountId == Key.AccountId;
				},
				OperationsToCancel);
		}
		for (const TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation : OperationsToCancel)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
	}

	void InvalidateAuthProfile(const FName AuthProfileId)
	{
		TArray<TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> OperationsToCancel;
		{
			FScopeLock Lock(&Mutex);
			for (TPair<FOAuthBindingKey, FBindingEntry> &Pair : Bindings)
			{
				if (Pair.Key.AuthProfileId == AuthProfileId)
				{
					for (TPair<FName, TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>>
							 &FreshnessPair : Pair.Value.ConnectionFreshness)
					{
						FreshnessPair.Value->Invalidate();
					}
					Pair.Value.ConnectionFreshness.Reset();
					++Pair.Value.Epoch;
					Pair.Value.bForceRefresh = false;
				}
			}
			CollectMatchingOperationsLocked([AuthProfileId](const FOAuthCredentialOperation &Operation)
											{ return Operation.GetSnapshot().Binding.AuthProfileId == AuthProfileId; },
											OperationsToCancel);
		}
		for (const TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation : OperationsToCancel)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
	}

	bool NotifyCredentialReplaced(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId,
								  const uint64 NewRevision, const double TimeoutSeconds,
								  const FUnrealAICancellationToken &Cancellation, FString &OutError)
	{
		OutError.Reset();
		if (AuthProfileId.IsNone() || !AccountId.IsValid() || NewRevision == 0 || !FMath::IsFinite(TimeoutSeconds) ||
			TimeoutSeconds <= 0.0 || !Cancellation.IsValid() || Cancellation.IsCancellationRequested())
		{
			OutError = TEXT(
				"OAuth credential replacement requires an exact account, nonzero revision, and live bounded context.");
			return false;
		}

		FUnrealAIOAuthCredentialBinding VerifiedBinding;
		{
			FScopeLock Lock(&Mutex);
			const FOAuthBindingKey Key{AuthProfileId, AccountId};
			const FBindingEntry *Entry = Bindings.Find(Key);
			if (bShutdown || Entry == nullptr)
			{
				OutError = bShutdown ? TEXT("OAuth broker is shut down.")
									 : TEXT("OAuth credential replacement has no registered account binding.");
				return false;
			}
			VerifiedBinding = Entry->Binding;
		}

		FUnrealAISecretStoreOperationContext StoreContext;
		FString ContextError;
		if (!FUnrealAISecretStoreOperationContext::TryCreate(
				Clock, FMath::Min(TimeoutSeconds, FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds),
				Cancellation, StoreContext, ContextError))
		{
			OutError = TEXT("OAuth credential replacement could not create its bounded verification context.");
			return false;
		}
		FUnrealAISecretValue EncodedEnvelope;
		uint64 ObservedRevision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult LoadResult = SecretStore->Load(StoreContext, VerifiedBinding.SecretHandle,
																		EncodedEnvelope, ObservedRevision, StoreError);
		if (LoadResult != EUnrealAISecretStoreResult::Succeeded || ObservedRevision != NewRevision ||
			StoreContext.IsCancellationRequested() || StoreContext.IsTimedOut())
		{
			EncodedEnvelope.Reset();
			OutError = LoadResult == EUnrealAISecretStoreResult::Succeeded
				? TEXT("OAuth credential replacement revision is no longer current.")
				: TEXT("OAuth credential replacement could not verify the secure-store record.");
			return false;
		}
		FUnrealAIOAuthTokenEnvelope VerifiedEnvelope;
		const FUnrealAIOAuthTokenEnvelopeBinding ExpectedBinding{
			VerifiedBinding.ProviderName, VerifiedBinding.AuthProfileId, VerifiedBinding.AccountId};
		FString DecodeError;
		if (!FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeForRefresh(MoveTemp(EncodedEnvelope), ExpectedBinding,
																   Clock->UtcNow(), VerifiedEnvelope, DecodeError) ||
			!VerifiedEnvelope.HasRefreshToken() ||
			(VerifiedBinding.bRequiresProtectedSecondary && !VerifiedEnvelope.HasAccountRoutingValue()) ||
			StoreContext.IsCancellationRequested() || StoreContext.IsTimedOut())
		{
			VerifiedEnvelope.Reset();
			OutError = TEXT("OAuth credential replacement material is invalid or incomplete.");
			return false;
		}
		VerifiedEnvelope.Reset();

		TArray<TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> OperationsToCancel;
		{
			FScopeLock Lock(&Mutex);
			const FOAuthBindingKey Key{AuthProfileId, AccountId};
			FBindingEntry *Entry = Bindings.Find(Key);
			if (bShutdown || Entry == nullptr)
			{
				OutError = bShutdown ? TEXT("OAuth broker is shut down.")
									 : TEXT("OAuth credential replacement has no registered account binding.");
				return false;
			}
			for (TPair<FName, TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> &Pair :
				 Entry->ConnectionFreshness)
			{
				Pair.Value->Invalidate();
			}
			Entry->ConnectionFreshness.Reset();
			Entry->QuarantineError.Reset();
			Entry->bForceRefresh = false;
			++Entry->Epoch;
			CollectMatchingOperationsLocked(
				[&Key](const FOAuthCredentialOperation &Operation)
				{
					return Operation.GetSnapshot().Binding.AuthProfileId == Key.AuthProfileId &&
						   Operation.GetSnapshot().Binding.AccountId == Key.AccountId;
				},
				OperationsToCancel);
		}
		for (const TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation : OperationsToCancel)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
		return true;
	}

	void QuarantineAccountForReauthentication(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId)
	{
		const FUnrealAIProviderAccessError Error = MakeOAuthBrokerAccessError(
			EUnrealAIErrorCategory::NotAuthorized, EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
		TArray<TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> OperationsToCancel;
		{
			FScopeLock Lock(&Mutex);
			const FOAuthBindingKey Key{AuthProfileId, AccountId};
			if (FBindingEntry *Entry = Bindings.Find(Key))
			{
				QuarantineEntryLocked(*Entry, Error);
				CollectMatchingOperationsLocked(
					[&Key](const FOAuthCredentialOperation &Operation)
					{
						return Operation.GetSnapshot().Binding.AuthProfileId == Key.AuthProfileId &&
							   Operation.GetSnapshot().Binding.AccountId == Key.AccountId;
					},
					OperationsToCancel);
			}
		}
		for (const TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation : OperationsToCancel)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
	}

	bool TryGetAccountQuarantine(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId,
								 FUnrealAIProviderAccessError &OutError) const
	{
		OutError = {};
		FScopeLock Lock(&Mutex);
		const FBindingEntry *Entry = Bindings.Find({AuthProfileId, AccountId});
		if (Entry == nullptr || !Entry->QuarantineError.IsSet())
		{
			return false;
		}
		OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::NotAuthorized,
											  EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
		return true;
	}

	void InvalidateConnection(const FName ConnectionAlias)
	{
		TArray<TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> OperationsToCancel;
		{
			FScopeLock Lock(&Mutex);
			++ConnectionEpochs.FindOrAdd(ConnectionAlias);
			for (TPair<FOAuthBindingKey, FBindingEntry> &Pair : Bindings)
			{
				if (TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> *Freshness =
						Pair.Value.ConnectionFreshness.Find(ConnectionAlias))
				{
					(*Freshness)->Invalidate();
					Pair.Value.ConnectionFreshness.Remove(ConnectionAlias);
				}
			}
			CollectMatchingOperationsLocked([ConnectionAlias](const FOAuthCredentialOperation &Operation)
											{ return Operation.GetConnection().ConnectionAlias == ConnectionAlias; },
											OperationsToCancel);
		}
		for (const TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation : OperationsToCancel)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
	}

	void ForceRefreshAccount(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId)
	{
		FScopeLock Lock(&Mutex);
		if (FBindingEntry *Entry = Bindings.Find({AuthProfileId, AccountId}))
		{
			Entry->bForceRefresh = true;
			for (TPair<FName, TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> &Pair :
				 Entry->ConnectionFreshness)
			{
				Pair.Value->Invalidate();
			}
			Entry->ConnectionFreshness.Reset();
			++Entry->Epoch;
		}
	}

	void BeginShutdown()
	{
		TArray<TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> OperationsToCancel;
		TArray<TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe>> FlightsToComplete;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown)
			{
				return;
			}
			bShutdown = true;
			for (TPair<FOAuthBindingKey, FBindingEntry> &Pair : Bindings)
			{
				for (TPair<FName, TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> &FreshnessPair :
					 Pair.Value.ConnectionFreshness)
				{
					FreshnessPair.Value->BeginShutdown();
				}
			}
			ActiveOperations.GenerateValueArray(OperationsToCancel);
			RefreshFlights.GenerateValueArray(FlightsToComplete);
			RefreshFlights.Reset();
		}
		const FUnrealAIProviderAccessError ShutdownError = MakeOAuthBrokerAccessError(
			EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed);
		for (const TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe> &Flight : FlightsToComplete)
		{
			Flight->Complete(false, ShutdownError);
		}
		for (const TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation : OperationsToCancel)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
	}

	bool IsShutdown() const
	{
		FScopeLock Lock(&Mutex);
		return bShutdown;
	}

	int32 GetActiveOperationCount() const
	{
		FScopeLock Lock(&Mutex);
		return ActiveOperations.Num();
	}

	int32 GetPhysicalOperationCount() const
	{
		FScopeLock Lock(&Mutex);
		return PhysicalOperationIds.Num();
	}

	int32 GetRefreshFlightCount() const
	{
		FScopeLock Lock(&Mutex);
		return RefreshFlights.Num();
	}

  private:
	void QuarantineEntryLocked(FBindingEntry &Entry, const FUnrealAIProviderAccessError &Error)
	{
		for (TPair<FName, TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> &Pair :
			 Entry.ConnectionFreshness)
		{
			Pair.Value->Invalidate();
		}
		Entry.ConnectionFreshness.Reset();
		Entry.QuarantineError = Error;
		Entry.bForceRefresh = false;
		++Entry.Epoch;
	}

	void QuarantineSnapshot(const FOAuthBindingSnapshot &Snapshot, const FUnrealAIProviderAccessError &Error)
	{
		const FOAuthBindingKey Key{Snapshot.Binding.AuthProfileId, Snapshot.Binding.AccountId};
		FScopeLock Lock(&Mutex);
		if (FBindingEntry *Entry = Bindings.Find(Key); Entry != nullptr && Entry->Epoch == Snapshot.BindingEpoch)
		{
			QuarantineEntryLocked(*Entry, Error);
		}
	}

	bool RefreshEnvelope(const TSharedRef<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation,
						 const FUnrealAISecretStoreOperationContext &StoreContext, const uint64 LoadedRevision,
						 FUnrealAIOAuthTokenEnvelope &InOutEnvelope)
	{
		if (!InOutEnvelope.HasRefreshToken())
		{
			const FUnrealAIProviderAccessError Error = MakeOAuthBrokerAccessError(
				EUnrealAIErrorCategory::NotAuthorized, EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
			QuarantineSnapshot(Operation->GetSnapshot(), Error);
			Operation->Publish(
				MakeOAuthBrokerTerminalResult(Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed, Error));
			return false;
		}
		const FOAuthBindingKey Key{Operation->GetSnapshot().Binding.AuthProfileId,
								   Operation->GetSnapshot().Binding.AccountId};
		bool bLeader = false;
		const TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe> Flight = AcquireRefreshFlight(Key, bLeader);
		if (!bLeader)
		{
			InOutEnvelope.Reset();
			while (!Flight->WaitSlice())
			{
				if (Operation->ObserveLogicalTerminal())
				{
					PublishInvalidated(Operation);
					return false;
				}
				if (!IsSnapshotCurrent(*Operation))
				{
					bool bCompleted = false;
					FUnrealAIProviderAccessError CompletedError;
					if (Flight->GetResult(bCompleted, CompletedError) && !bCompleted && CompletedError.IsError())
					{
						Operation->Publish(MakeOAuthBrokerTerminalResult(
							Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed, CompletedError));
					}
					else
					{
						PublishInvalidated(Operation);
					}
					return false;
				}
			}
			bool bSucceeded = false;
			FUnrealAIProviderAccessError RefreshError;
			if (!Flight->GetResult(bSucceeded, RefreshError) || !bSucceeded)
			{
				Operation->Publish(MakeOAuthBrokerTerminalResult(
					Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed,
					RefreshError.IsError()
						? RefreshError
						: MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
													 EUnrealAIProviderAccessErrorCode::CredentialFailed, true)));
				return false;
			}
			return ReloadEnvelope(Operation, StoreContext, InOutEnvelope);
		}

		FUnrealAIProviderAccessError RefreshError;
		bool bSucceeded = false;
		bool bQuarantine = false;
		if (!Operation->ObserveLogicalTerminal() && IsSnapshotCurrent(*Operation))
		{
			bSucceeded = RefreshSource->Refresh(InOutEnvelope, Operation->RemainingSeconds(),
												Operation->GetCancellationToken(), RefreshError);
			bQuarantine = !bSucceeded && IsQuarantiningRefreshError(RefreshError);
		}
		const bool bMayCommit = bSucceeded && !Operation->ObserveLogicalTerminal() && IsSnapshotCurrent(*Operation);
		if (bMayCommit)
		{
			FUnrealAISecretValue Encoded;
			FString EncodeError;
			if (!FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(InOutEnvelope), Operation->GetClock().UtcNow(),
															 Encoded, EncodeError))
			{
				bSucceeded = false;
				RefreshError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
														  EUnrealAIProviderAccessErrorCode::CredentialFailed);
			}
			else
			{
				uint64 NewRevision = 0;
				FUnrealAIProviderAccessError StoreError;
				const EUnrealAISecretStoreResult StoreResult =
					SecretStore->Store(StoreContext, Operation->GetSnapshot().Binding.SecretHandle, Encoded,
									   LoadedRevision, NewRevision, StoreError);
				if (StoreResult == EUnrealAISecretStoreResult::Succeeded && NewRevision != 0 &&
					IsSnapshotCurrent(*Operation) && !Operation->ObserveLogicalTerminal())
				{
					const FUnrealAIOAuthTokenEnvelopeBinding ExpectedBinding{
						Operation->GetSnapshot().Binding.ProviderName, Operation->GetSnapshot().Binding.AuthProfileId,
						Operation->GetSnapshot().Binding.AccountId};
					FString DecodeError;
					bSucceeded = FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeForRefresh(
						MoveTemp(Encoded), ExpectedBinding, Operation->GetClock().UtcNow(), InOutEnvelope, DecodeError);
					if (!bSucceeded)
					{
						RefreshError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Persistence,
																  EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
					}
					else
					{
						ClearForceRefresh(Operation->GetSnapshot());
					}
				}
				else if (StoreResult == EUnrealAISecretStoreResult::Conflict && IsSnapshotCurrent(*Operation) &&
						 !Operation->ObserveLogicalTerminal())
				{
					Encoded.Reset();
					FUnrealAIOAuthTokenEnvelope WinningEnvelope;
					FUnrealAIProviderAccessError WinningError;
					if (TryReloadFreshEnvelope(Operation, StoreContext, WinningEnvelope, WinningError))
					{
						InOutEnvelope = MoveTemp(WinningEnvelope);
						bSucceeded = true;
						RefreshError = {};
						ClearForceRefresh(Operation->GetSnapshot());
					}
					else
					{
						bSucceeded = false;
						const FUnrealAICredentialResult StoreFailure =
							MakeOAuthBrokerStoreFailure(Operation->GetRequestId(), StoreResult, StoreError);
						RefreshError = StoreFailure.Error;
					}
				}
				else
				{
					Encoded.Reset();
					bSucceeded = false;
					const FUnrealAICredentialResult StoreFailure =
						MakeOAuthBrokerStoreFailure(Operation->GetRequestId(), StoreResult, StoreError);
					RefreshError = StoreFailure.Error;
				}
			}
		}
		else
		{
			bSucceeded = false;
			if (!RefreshError.IsError())
			{
				RefreshError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
														  EUnrealAIProviderAccessErrorCode::CredentialFailed, true);
			}
		}
		CompleteRefreshFlight(Key, Flight, bSucceeded, RefreshError, bQuarantine ? &Operation->GetSnapshot() : nullptr);
		if (Operation->ObserveLogicalTerminal())
		{
			InOutEnvelope.Reset();
			return false;
		}
		if (!bSucceeded)
		{
			Operation->Publish(MakeOAuthBrokerTerminalResult(Operation->GetRequestId(),
															 EUnrealAICredentialResultKind::Failed, RefreshError));
			return false;
		}
		return true;
	}

	bool TryReloadFreshEnvelope(const TSharedRef<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation,
								const FUnrealAISecretStoreOperationContext &StoreContext,
								FUnrealAIOAuthTokenEnvelope &OutEnvelope, FUnrealAIProviderAccessError &OutError)
	{
		OutEnvelope.Reset();
		OutError = {};
		FUnrealAISecretValue Encoded;
		uint64 Revision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult Result = SecretStore->Load(
			StoreContext, Operation->GetSnapshot().Binding.SecretHandle, Encoded, Revision, StoreError);
		if (Result != EUnrealAISecretStoreResult::Succeeded || Revision == 0)
		{
			FUnrealAICredentialResult Failure =
				MakeOAuthBrokerStoreFailure(Operation->GetRequestId(), Result, StoreError);
			OutError = Failure.Error;
			if (Result == EUnrealAISecretStoreResult::NotFound || Result == EUnrealAISecretStoreResult::Corrupt ||
				(Result == EUnrealAISecretStoreResult::Succeeded && Revision == 0))
			{
				QuarantineSnapshot(Operation->GetSnapshot(), OutError);
			}
			return false;
		}
		const FUnrealAIOAuthTokenEnvelopeBinding ExpectedBinding{Operation->GetSnapshot().Binding.ProviderName,
																 Operation->GetSnapshot().Binding.AuthProfileId,
																 Operation->GetSnapshot().Binding.AccountId};
		FString DecodeError;
		if (!FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeForRefresh(
				MoveTemp(Encoded), ExpectedBinding, Operation->GetClock().UtcNow(), OutEnvelope, DecodeError))
		{
			OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Persistence,
												  EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
			QuarantineSnapshot(Operation->GetSnapshot(), OutError);
			return false;
		}
		if (!OutEnvelope.HasRefreshToken() ||
			(Operation->GetSnapshot().Binding.bRequiresProtectedSecondary && !OutEnvelope.HasAccountRoutingValue()))
		{
			OutEnvelope.Reset();
			OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::NotAuthorized,
												  EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
			QuarantineSnapshot(Operation->GetSnapshot(), OutError);
			return false;
		}
		if (OutEnvelope.GetAccessTokenExpiresAtUtc() <=
			Operation->GetClock().UtcNow() + FTimespan::FromSeconds(Config.RefreshSkewSeconds))
		{
			OutEnvelope.Reset();
			OutError = MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
												  EUnrealAIProviderAccessErrorCode::CredentialFailed, true);
			return false;
		}
		return true;
	}

	bool ReloadEnvelope(const TSharedRef<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation,
						const FUnrealAISecretStoreOperationContext &StoreContext,
						FUnrealAIOAuthTokenEnvelope &OutEnvelope)
	{
		FUnrealAIProviderAccessError Error;
		if (TryReloadFreshEnvelope(Operation, StoreContext, OutEnvelope, Error))
		{
			return true;
		}
		Operation->Publish(MakeOAuthBrokerTerminalResult(
			Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed,
			Error.IsError() ? Error
							: MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
														 EUnrealAIProviderAccessErrorCode::CredentialFailed, true)));
		return false;
	}

	void PublishLease(const TSharedRef<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation,
					  FUnrealAIOAuthTokenEnvelope &&Envelope)
	{
		if (Operation->ObserveLogicalTerminal() || !IsSnapshotCurrent(*Operation))
		{
			Envelope.Reset();
			PublishInvalidated(Operation);
			return;
		}
		const FDateTime Expiry = Envelope.GetAccessTokenExpiresAtUtc();
		FUnrealAISecretValue Bearer;
		FUnrealAISecretValue ProtectedSecondary;
		FString DispatchError;
		if (!Envelope.TryTakeDispatchCredentials(Operation->GetClock().UtcNow(), Bearer, ProtectedSecondary,
												 DispatchError))
		{
			Operation->Publish(MakeOAuthBrokerTerminalResult(
				Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed,
				MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::Provider,
										   EUnrealAIProviderAccessErrorCode::CredentialFailed)));
			return;
		}
		FUnrealAICredentialLease Lease;
		bool bLeaseCreated = false;
		if (Operation->GetSnapshot().Binding.bRequiresProtectedSecondary)
		{
			bLeaseCreated = FUnrealAICredentialLease::TryCreateWithProtectedSecondary(
				Operation->GetConnection().CredentialDestination, Clock, Operation->GetSnapshot().Freshness->GetToken(),
				Config.LeaseLifetimeSeconds, Expiry, MoveTemp(Bearer), MoveTemp(ProtectedSecondary), Lease,
				DispatchError);
		}
		else
		{
			ProtectedSecondary.Reset();
			bLeaseCreated = FUnrealAICredentialLease::TryCreate(
				Operation->GetConnection().CredentialDestination, Clock, Operation->GetSnapshot().Freshness->GetToken(),
				Config.LeaseLifetimeSeconds, Expiry, MoveTemp(Bearer), Lease, DispatchError);
		}
		if (!bLeaseCreated || !IsSnapshotCurrent(*Operation))
		{
			Lease.Reset();
			Operation->Publish(MakeOAuthBrokerTerminalResult(
				Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed,
				MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::NotAuthorized,
										   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady)));
			return;
		}
		FUnrealAICredentialResult Result;
		Result.RequestId = Operation->GetRequestId();
		Result.Kind = EUnrealAICredentialResultKind::Succeeded;
		Result.AccessContext = IUnrealAIProviderAccessContext::CreateCredentialed(MoveTemp(Lease));
		Operation->Publish(MoveTemp(Result));
	}

	bool IsSnapshotCurrent(const FOAuthCredentialOperation &Operation) const
	{
		const FOAuthBindingSnapshot &Snapshot = Operation.GetSnapshot();
		const FOAuthBindingKey Key{Snapshot.Binding.AuthProfileId, Snapshot.Binding.AccountId};
		FScopeLock Lock(&Mutex);
		const FBindingEntry *Entry = Bindings.Find(Key);
		const TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> *CurrentFreshness =
			Entry != nullptr ? Entry->ConnectionFreshness.Find(Operation.GetConnection().ConnectionAlias) : nullptr;
		return !bShutdown && Entry != nullptr && Entry->Epoch == Snapshot.BindingEpoch && CurrentFreshness != nullptr &&
			   CurrentFreshness->ToSharedPtr() == Snapshot.Freshness &&
			   ConnectionEpochs.FindRef(Operation.GetConnection().ConnectionAlias) == Snapshot.ConnectionEpoch &&
			   Snapshot.Freshness.IsValid() && Snapshot.Freshness->GetToken().IsCurrent();
	}

	void PublishInvalidated(const TSharedRef<FOAuthCredentialOperation, ESPMode::ThreadSafe> &Operation)
	{
		if (!Operation->IsComplete())
		{
			Operation->Publish(MakeOAuthBrokerTerminalResult(
				Operation->GetRequestId(), EUnrealAICredentialResultKind::Failed,
				MakeOAuthBrokerAccessError(EUnrealAIErrorCategory::NotAuthorized,
										   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady)));
		}
	}

	TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe> AcquireRefreshFlight(const FOAuthBindingKey &Key,
																			  bool &bOutLeader)
	{
		FScopeLock Lock(&Mutex);
		if (const TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe> *Existing = RefreshFlights.Find(Key))
		{
			bOutLeader = false;
			return *Existing;
		}
		const TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe> Created =
			MakeShared<FOAuthRefreshFlight, ESPMode::ThreadSafe>();
		RefreshFlights.Add(Key, Created);
		bOutLeader = true;
		return Created;
	}

	void CompleteRefreshFlight(const FOAuthBindingKey &Key,
							   const TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe> &Flight,
							   const bool bSucceeded, const FUnrealAIProviderAccessError &Error,
							   const FOAuthBindingSnapshot *QuarantineSnapshot = nullptr)
	{
		FScopeLock Lock(&Mutex);
		if (QuarantineSnapshot != nullptr)
		{
			FBindingEntry *Entry = Bindings.Find(Key);
			if (Entry != nullptr && Entry->Epoch == QuarantineSnapshot->BindingEpoch)
			{
				QuarantineEntryLocked(*Entry, Error);
			}
		}
		Flight->Complete(bSucceeded, Error);
		const TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe> *Current = RefreshFlights.Find(Key);
		if (Current != nullptr && &Current->Get() == &Flight.Get())
		{
			RefreshFlights.Remove(Key);
		}
	}

	void ClearForceRefresh(const FOAuthBindingSnapshot &Snapshot)
	{
		const FOAuthBindingKey Key{Snapshot.Binding.AuthProfileId, Snapshot.Binding.AccountId};
		FScopeLock Lock(&Mutex);
		if (FBindingEntry *Entry = Bindings.Find(Key); Entry != nullptr && Entry->Epoch == Snapshot.BindingEpoch)
		{
			Entry->bForceRefresh = false;
		}
	}

	void RemoveOperation(const FUnrealAIRequestId &RequestId)
	{
		FScopeLock Lock(&Mutex);
		ActiveOperations.Remove(RequestId.Value);
	}

	void RemovePhysicalOperation(const FUnrealAIRequestId &RequestId)
	{
		FScopeLock Lock(&Mutex);
		PhysicalOperationIds.Remove(RequestId.Value);
	}

	template <typename PredicateType>
	void CollectMatchingOperationsLocked(PredicateType Predicate,
										 TArray<TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> &Out)
	{
		for (const TPair<FGuid, TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> &Pair : ActiveOperations)
		{
			if (Pair.Value.IsValid() && Predicate(*Pair.Value))
			{
				Out.Add(Pair.Value);
			}
		}
	}

	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedRef<IUnrealAIOAuthCredentialRefreshSource, ESPMode::ThreadSafe> RefreshSource;
	FUnrealAIOAuthCredentialBrokerConfig Config;
	mutable FCriticalSection Mutex;
	TMap<FOAuthBindingKey, FBindingEntry> Bindings;
	TMap<FName, uint64> ConnectionEpochs;
	TMap<FGuid, TSharedPtr<FOAuthCredentialOperation, ESPMode::ThreadSafe>> ActiveOperations;
	TSet<FGuid> PhysicalOperationIds;
	TMap<FOAuthBindingKey, TSharedRef<FOAuthRefreshFlight, ESPMode::ThreadSafe>> RefreshFlights;
	bool bShutdown = false;
};
} // namespace UE::UnrealAI::Private

bool FUnrealAIOAuthCredentialBinding::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FUnrealAIOAuthTokenEnvelopeBinding EnvelopeBinding{ProviderName, AuthProfileId, AccountId};
	if (!EnvelopeBinding.ValidateShape(OutError) || !SecretHandle.ValidateShape(OutError))
	{
		return false;
	}
	return true;
}

bool FUnrealAIOAuthCredentialBrokerConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (MaxConcurrentOperations < 1 || MaxConcurrentOperations > HardMaxConcurrentOperations ||
		!FMath::IsFinite(LeaseLifetimeSeconds) || LeaseLifetimeSeconds <= 0.0 ||
		LeaseLifetimeSeconds > FUnrealAICredentialLease::MaxLeaseLifetimeSeconds ||
		!FMath::IsFinite(RefreshSkewSeconds) || RefreshSkewSeconds < 0.0 || RefreshSkewSeconds > MaxRefreshSkewSeconds)
	{
		OutError = TEXT("OAuth credential broker configuration is outside its compiled resource bounds.");
		return false;
	}
	return true;
}

FUnrealAIOAuthCredentialBroker::FUnrealAIOAuthCredentialBroker(
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	TSharedRef<IUnrealAIOAuthCredentialRefreshSource, ESPMode::ThreadSafe> InRefreshSource,
	const FUnrealAIOAuthCredentialBrokerConfig &InConfig)
	: State(MakeShared<UE::UnrealAI::Private::FOAuthCredentialBrokerState, ESPMode::ThreadSafe>(
		  MoveTemp(InConnections), MoveTemp(InSecretStore), MoveTemp(InClock), MoveTemp(InRefreshSource), InConfig))
{
}

FUnrealAIOAuthCredentialBroker::~FUnrealAIOAuthCredentialBroker()
{
	BeginShutdown();
}

bool FUnrealAIOAuthCredentialBroker::RegisterBinding(const FUnrealAIOAuthCredentialBinding &Binding, FString &OutError)
{
	return State->RegisterBinding(Binding, OutError);
}

bool FUnrealAIOAuthCredentialBroker::StartResolve(
	const FUnrealAICredentialRequest &Request, TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink,
	const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
	FUnrealAIProviderAccessError &OutError)
{
	return State->StartResolve(Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

void FUnrealAIOAuthCredentialBroker::InvalidateAccount(const FName AuthProfileId,
													   const FUnrealAIAccessAccountId &AccountId)
{
	State->InvalidateAccount(AuthProfileId, AccountId);
}

void FUnrealAIOAuthCredentialBroker::InvalidateAuthProfile(const FName AuthProfileId)
{
	State->InvalidateAuthProfile(AuthProfileId);
}

void FUnrealAIOAuthCredentialBroker::InvalidateConnection(const FName ConnectionAlias)
{
	State->InvalidateConnection(ConnectionAlias);
}

bool FUnrealAIOAuthCredentialBroker::NotifyCredentialReplaced(const FName AuthProfileId,
															  const FUnrealAIAccessAccountId &AccountId,
															  const uint64 NewRevision, const double TimeoutSeconds,
															  const FUnrealAICancellationToken &Cancellation,
															  FString &OutError)
{
	return State->NotifyCredentialReplaced(AuthProfileId, AccountId, NewRevision, TimeoutSeconds, Cancellation,
										   OutError);
}

void FUnrealAIOAuthCredentialBroker::QuarantineAccountForReauthentication(const FName AuthProfileId,
																		  const FUnrealAIAccessAccountId &AccountId)
{
	State->QuarantineAccountForReauthentication(AuthProfileId, AccountId);
}

bool FUnrealAIOAuthCredentialBroker::TryGetAccountQuarantine(const FName AuthProfileId,
															 const FUnrealAIAccessAccountId &AccountId,
															 FUnrealAIProviderAccessError &OutError) const
{
	return State->TryGetAccountQuarantine(AuthProfileId, AccountId, OutError);
}

void FUnrealAIOAuthCredentialBroker::BeginShutdown()
{
	State->BeginShutdown();
}

void FUnrealAIOAuthCredentialBroker::ForceRefreshAccount(const FName AuthProfileId,
														 const FUnrealAIAccessAccountId &AccountId)
{
	State->ForceRefreshAccount(AuthProfileId, AccountId);
}

bool FUnrealAIOAuthCredentialBroker::IsShutdown() const
{
	return State->IsShutdown();
}

int32 FUnrealAIOAuthCredentialBroker::GetActiveOperationCount() const
{
	return State->GetActiveOperationCount();
}

int32 FUnrealAIOAuthCredentialBroker::GetPhysicalOperationCount() const
{
	return State->GetPhysicalOperationCount();
}

int32 FUnrealAIOAuthCredentialBroker::GetRefreshFlightCount() const
{
	return State->GetRefreshFlightCount();
}
