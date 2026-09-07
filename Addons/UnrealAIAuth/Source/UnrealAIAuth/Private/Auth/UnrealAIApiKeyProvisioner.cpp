// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIApiKeyProvisioner.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

namespace
{
FUnrealAIProviderAccessError MakeApiKeyProvisionError(const EUnrealAIErrorCategory Category,
													  const EUnrealAIProviderAccessErrorCode Code,
													  const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

FUnrealAIApiKeyProvisionResult MakeResult(const FUnrealAIRequestId RequestId, const EUnrealAIApiKeyProvisionKind Kind,
										  FUnrealAIProviderAccessError Error = {})
{
	FUnrealAIApiKeyProvisionResult Result;
	Result.RequestId = RequestId;
	Result.Kind = Kind;
	Result.Error = Error;
	return Result;
}
} // namespace

bool FUnrealAIApiKeyProvisionerConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString HandleError;
	if (AuthProfileId.IsNone() || !AccountId.IsValid() || !SecretHandle.ValidateShape(HandleError) ||
		MaximumConcurrentOperations < 1 || MaximumConcurrentOperations > MaximumConcurrentOperationsLimit)
	{
		OutError = TEXT("API-key provisioner configuration is invalid or exceeds framework bounds.");
		return false;
	}
	return true;
}

class FUnrealAIApiKeyProvisionOperation final
	: public IUnrealAIApiKeyProvisionHandle,
	  public TSharedFromThis<FUnrealAIApiKeyProvisionOperation, ESPMode::ThreadSafe>
{
  public:
	FUnrealAIApiKeyProvisionOperation(const FUnrealAIRequestId InRequestId, const bool bInDelete,
									  FUnrealAISecretValue &&InSecret, const double TimeoutSeconds,
									  const FUnrealAISecretHandle &InSecretHandle,
									  TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InStore,
									  TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
									  TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> InSink,
									  const FUnrealAICancellationToken &ParentCancellation,
									  TFunction<void()> InOnCredentialChanged,
									  TFunction<void(const FUnrealAIRequestId &)> InOnTerminal)
		: RequestId(InRequestId), bDelete(bInDelete), SecretHandle(InSecretHandle), PendingSecret(MoveTemp(InSecret)),
		  Store(MoveTemp(InStore)), Clock(MoveTemp(InClock)), Cancellation(ParentCancellation),
		  Deadline(FUnrealAIDeadline::FromNow(*Clock, TimeoutSeconds)), Sink(InSink.ToSharedPtr()),
		  OnCredentialChanged(MoveTemp(InOnCredentialChanged)), OnTerminal(MoveTemp(InOnTerminal))
	{
		FString ContextError;
		bContextValid = FUnrealAISecretStoreOperationContext::TryCreate(
			Clock, FMath::Min(TimeoutSeconds, FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds),
			Cancellation.GetToken(), StoreContext, ContextError);
	}

	~FUnrealAIApiKeyProvisionOperation() override
	{
		WipePendingSecret();
	}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}

	void Cancel() override
	{
		Cancellation.Cancel(EUnrealAICancellationReason::Requested);
		WipePendingSecret();
		Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::Cancelled,
						   MakeApiKeyProvisionError(EUnrealAIErrorCategory::Cancelled,
													EUnrealAIProviderAccessErrorCode::SecretStoreCancelled)),
				EUnrealAITerminalKind::Cancelled);
	}

	bool IsContextValid() const
	{
		return bContextValid;
	}

	void Run()
	{
		ObserveLogicalTerminal();
		if (Terminal.IsComplete())
		{
			WipePendingSecret();
			return;
		}

		FUnrealAISecretValue Existing;
		uint64 Revision = 0;
		FUnrealAIProviderAccessError StoreError;
		const EUnrealAISecretStoreResult LoadResult =
			Store->Load(StoreContext, SecretHandle, Existing, Revision, StoreError);
		Existing.Reset();
		ObserveLogicalTerminal();
		if (Terminal.IsComplete())
		{
			WipePendingSecret();
			return;
		}

		if (bDelete)
		{
			WipePendingSecret();
			if (LoadResult == EUnrealAISecretStoreResult::NotFound)
			{
				CredentialChanged();
				Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::Deleted), EUnrealAITerminalKind::Succeeded);
				return;
			}
			if (LoadResult != EUnrealAISecretStoreResult::Succeeded || Revision == 0)
			{
				PublishStoreFailure(LoadResult, StoreError);
				return;
			}
			const EUnrealAISecretStoreResult DeleteResult =
				Store->Delete(StoreContext, SecretHandle, Revision, StoreError);
			// A platform mutation can commit before its backend observes cancellation or timeout. Invalidate after
			// every issued call so a logically failed/indeterminate delete cannot leave a pre-mutation lease usable.
			CredentialChanged();
			ObserveLogicalTerminal();
			if (Terminal.IsComplete())
			{
				return;
			}
			if (DeleteResult == EUnrealAISecretStoreResult::Succeeded ||
				DeleteResult == EUnrealAISecretStoreResult::NotFound)
			{
				Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::Deleted), EUnrealAITerminalKind::Succeeded);
			}
			else
			{
				PublishStoreFailure(DeleteResult, StoreError);
			}
			return;
		}

		if (LoadResult != EUnrealAISecretStoreResult::Succeeded && LoadResult != EUnrealAISecretStoreResult::NotFound)
		{
			WipePendingSecret();
			PublishStoreFailure(LoadResult, StoreError);
			return;
		}
		FUnrealAISecretValue Secret;
		if (!TakePendingSecret(Secret))
		{
			Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::Cancelled,
							   MakeApiKeyProvisionError(EUnrealAIErrorCategory::Cancelled,
														EUnrealAIProviderAccessErrorCode::SecretStoreCancelled)),
					EUnrealAITerminalKind::Cancelled);
			return;
		}
		uint64 NewRevision = 0;
		const EUnrealAISecretStoreResult StoreResult =
			Store->Store(StoreContext, SecretHandle, Secret,
						 LoadResult == EUnrealAISecretStoreResult::Succeeded ? Revision : 0, NewRevision, StoreError);
		Secret.Reset();
		// As with delete, a cancelled/timed-out/error return does not prove that the physical write was rolled back.
		CredentialChanged();
		ObserveLogicalTerminal();
		if (Terminal.IsComplete())
		{
			return;
		}
		if (StoreResult == EUnrealAISecretStoreResult::Succeeded && NewRevision != 0)
		{
			Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::Stored), EUnrealAITerminalKind::Succeeded);
		}
		else
		{
			PublishStoreFailure(StoreResult, StoreError);
		}
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

  private:
	void ObserveLogicalTerminal()
	{
		if (Terminal.IsComplete())
		{
			return;
		}
		const FUnrealAICancellationToken Token = Cancellation.GetToken();
		if (Token.IsCancellationRequested())
		{
			WipePendingSecret();
			if (Token.GetReason() == EUnrealAICancellationReason::Timeout)
			{
				Publish(
					MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::TimedOut,
							   MakeApiKeyProvisionError(EUnrealAIErrorCategory::Timeout,
														EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut, true)),
					EUnrealAITerminalKind::TimedOut);
			}
			else
			{
				Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::Cancelled,
								   MakeApiKeyProvisionError(EUnrealAIErrorCategory::Cancelled,
															EUnrealAIProviderAccessErrorCode::SecretStoreCancelled)),
						EUnrealAITerminalKind::Cancelled);
			}
		}
		else if (Deadline.IsExpired(*Clock))
		{
			Cancellation.Cancel(EUnrealAICancellationReason::Timeout);
			WipePendingSecret();
			Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::TimedOut,
							   MakeApiKeyProvisionError(EUnrealAIErrorCategory::Timeout,
														EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut, true)),
					EUnrealAITerminalKind::TimedOut);
		}
	}

	void PublishStoreFailure(const EUnrealAISecretStoreResult Result, const FUnrealAIProviderAccessError &StoreError)
	{
		if (Result == EUnrealAISecretStoreResult::Cancelled)
		{
			Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::Cancelled, StoreError),
					EUnrealAITerminalKind::Cancelled);
		}
		else if (Result == EUnrealAISecretStoreResult::TimedOut)
		{
			Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::TimedOut, StoreError),
					EUnrealAITerminalKind::TimedOut);
		}
		else
		{
			const FUnrealAIProviderAccessError Error =
				StoreError.IsError() ? StoreError
									 : MakeApiKeyProvisionError(EUnrealAIErrorCategory::Persistence,
																EUnrealAIProviderAccessErrorCode::CredentialFailed);
			Publish(MakeResult(RequestId, EUnrealAIApiKeyProvisionKind::Failed, Error), EUnrealAITerminalKind::Failed);
		}
	}

	bool Publish(FUnrealAIApiKeyProvisionResult &&Result, const EUnrealAITerminalKind Kind)
	{
		if (!Terminal.TryComplete(Kind))
		{
			return false;
		}
		TSharedPtr<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> LocalSink;
		{
			FScopeLock Lock(&SinkMutex);
			LocalSink = MoveTemp(Sink);
		}
		if (LocalSink.IsValid())
		{
			LocalSink->EnqueueApiKeyProvisionResult(MoveTemp(Result));
		}
		if (OnTerminal)
		{
			OnTerminal(RequestId);
		}
		return true;
	}

	bool TakePendingSecret(FUnrealAISecretValue &OutSecret)
	{
		FScopeLock Lock(&SecretMutex);
		if (!PendingSecret.IsSet())
		{
			return false;
		}
		OutSecret = MoveTemp(PendingSecret);
		return OutSecret.IsSet();
	}

	void WipePendingSecret()
	{
		FScopeLock Lock(&SecretMutex);
		PendingSecret.Reset();
	}

	void CredentialChanged()
	{
		if (OnCredentialChanged)
		{
			OnCredentialChanged();
		}
	}

	FUnrealAIRequestId RequestId;
	bool bDelete = false;
	FUnrealAISecretHandle SecretHandle;
	FCriticalSection SecretMutex;
	FUnrealAISecretValue PendingSecret;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAICancellationSource Cancellation;
	FUnrealAIDeadline Deadline;
	FUnrealAISecretStoreOperationContext StoreContext;
	bool bContextValid = false;
	FCriticalSection SinkMutex;
	TSharedPtr<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink;
	TFunction<void()> OnCredentialChanged;
	TFunction<void(const FUnrealAIRequestId &)> OnTerminal;
	FUnrealAITerminalGuard Terminal;
};

class FUnrealAIApiKeyProvisioner::FState final : public TSharedFromThis<FState, ESPMode::ThreadSafe>
{
  public:
	FState(FUnrealAIApiKeyProvisionerConfig InConfig,
		   TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		   TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		   TSharedRef<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> InBroker)
		: Config(MoveTemp(InConfig)), SecretStore(MoveTemp(InSecretStore)), Clock(MoveTemp(InClock)),
		  Broker(MoveTemp(InBroker))
	{
	}

	void Remove(const FUnrealAIRequestId &RequestId)
	{
		FScopeLock Lock(&Mutex);
		Operations.Remove(RequestId.Value);
	}

	void BeginShutdown()
	{
		TArray<TSharedPtr<FUnrealAIApiKeyProvisionOperation, ESPMode::ThreadSafe>> Pending;
		TSharedPtr<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> BrokerToInvalidate;
		{
			FScopeLock Lock(&Mutex);
			if (bShuttingDown)
			{
				return;
			}
			bShuttingDown = true;
			Operations.GenerateValueArray(Pending);
			BrokerToInvalidate = Broker;
			Broker.Reset();
		}
		if (BrokerToInvalidate.IsValid())
		{
			BrokerToInvalidate->InvalidateAccount(Config.AuthProfileId, Config.AccountId);
		}
		for (const TSharedPtr<FUnrealAIApiKeyProvisionOperation, ESPMode::ThreadSafe> &Operation : Pending)
		{
			if (Operation.IsValid())
			{
				Operation->Cancel();
			}
		}
	}

	mutable FCriticalSection Mutex;
	FUnrealAIApiKeyProvisionerConfig Config;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedPtr<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> Broker;
	TMap<FGuid, TSharedPtr<FUnrealAIApiKeyProvisionOperation, ESPMode::ThreadSafe>> Operations;
	bool bShuttingDown = false;
};

FUnrealAIApiKeyProvisioner::FUnrealAIApiKeyProvisioner(TSharedRef<FState, ESPMode::ThreadSafe> InState)
	: State(MoveTemp(InState))
{
}

FUnrealAIApiKeyProvisioner::~FUnrealAIApiKeyProvisioner()
{
	BeginShutdown();
}

bool FUnrealAIApiKeyProvisioner::TryCreate(const FUnrealAIApiKeyProvisionerConfig &Config,
										   TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore,
										   TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
										   TSharedRef<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> Broker,
										   TSharedPtr<FUnrealAIApiKeyProvisioner, ESPMode::ThreadSafe> &OutProvisioner,
										   FString &OutError)
{
	OutProvisioner.Reset();
	OutError.Reset();
	FString CapabilityError;
	if (!Config.ValidateShape(OutError) || !SecretStore->DescribeCapabilities().ValidateShape(CapabilityError) ||
		!SecretStore->DescribeCapabilities().bAvailableInCurrentBuild)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("API-key provisioner requires an available secure-store backend.");
		}
		return false;
	}
	const TSharedRef<FState, ESPMode::ThreadSafe> NewState =
		MakeShared<FState, ESPMode::ThreadSafe>(Config, SecretStore, Clock, Broker);
	OutProvisioner =
		TSharedPtr<FUnrealAIApiKeyProvisioner, ESPMode::ThreadSafe>(new FUnrealAIApiKeyProvisioner(NewState));
	return true;
}

bool FUnrealAIApiKeyProvisioner::StartStore(const FUnrealAIRequestId &RequestId, FUnrealAISecretValue &&ApiKey,
											const float TimeoutSeconds,
											TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink,
											const FUnrealAICancellationToken &Cancellation,
											TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> &OutHandle,
											FUnrealAIProviderAccessError &OutError)
{
	if (!ApiKey.IsSet())
	{
		OutHandle.Reset();
		OutError = MakeApiKeyProvisionError(EUnrealAIErrorCategory::InvalidArgument,
											EUnrealAIProviderAccessErrorCode::InvalidSecretStoreWrite);
		return false;
	}
	return Start(RequestId, false, MoveTemp(ApiKey), TimeoutSeconds, MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

bool FUnrealAIApiKeyProvisioner::StartDelete(const FUnrealAIRequestId &RequestId, const float TimeoutSeconds,
											 TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink,
											 const FUnrealAICancellationToken &Cancellation,
											 TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> &OutHandle,
											 FUnrealAIProviderAccessError &OutError)
{
	FUnrealAISecretValue Empty;
	return Start(RequestId, true, MoveTemp(Empty), TimeoutSeconds, MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

bool FUnrealAIApiKeyProvisioner::Start(const FUnrealAIRequestId &RequestId, const bool bDelete,
									   FUnrealAISecretValue &&Secret, const float TimeoutSeconds,
									   TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink,
									   const FUnrealAICancellationToken &Cancellation,
									   TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> &OutHandle,
									   FUnrealAIProviderAccessError &OutError)
{
	OutHandle.Reset();
	OutError = {};
	if (!RequestId.IsValid() || !Cancellation.IsValid() || !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0f ||
		TimeoutSeconds > FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds)
	{
		OutError = MakeApiKeyProvisionError(EUnrealAIErrorCategory::InvalidArgument,
											EUnrealAIProviderAccessErrorCode::InvalidSecretStoreContext);
		return false;
	}
	if (Cancellation.IsCancellationRequested())
	{
		OutError = MakeApiKeyProvisionError(EUnrealAIErrorCategory::Cancelled,
											EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
		return false;
	}

	TSharedPtr<FUnrealAIApiKeyProvisionOperation, ESPMode::ThreadSafe> Operation;
	{
		FScopeLock Lock(&State->Mutex);
		if (State->bShuttingDown || !State->Broker.IsValid() ||
			State->Operations.Num() >= State->Config.MaximumConcurrentOperations ||
			State->Operations.Contains(RequestId.Value))
		{
			OutError = MakeApiKeyProvisionError(EUnrealAIErrorCategory::Busy,
												EUnrealAIProviderAccessErrorCode::SecretStoreCapacity, true);
			return false;
		}
		TWeakPtr<FState, ESPMode::ThreadSafe> WeakState = State;
		const TWeakPtr<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> WeakBroker = State->Broker;
		const FName AuthProfileId = State->Config.AuthProfileId;
		const FUnrealAIAccessAccountId AccountId = State->Config.AccountId;
		Operation = MakeShared<FUnrealAIApiKeyProvisionOperation, ESPMode::ThreadSafe>(
			RequestId, bDelete, MoveTemp(Secret), TimeoutSeconds, State->Config.SecretHandle, State->SecretStore,
			State->Clock, Sink, Cancellation,
			[WeakBroker, AuthProfileId, AccountId]()
			{
				if (const TSharedPtr<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> Pinned = WeakBroker.Pin())
				{
					Pinned->InvalidateAccount(AuthProfileId, AccountId);
				}
			},
			[WeakState](const FUnrealAIRequestId &CompletedRequest)
			{
				if (const TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakState.Pin())
				{
					Pinned->Remove(CompletedRequest);
				}
			});
		if (!Operation->IsContextValid())
		{
			OutError = MakeApiKeyProvisionError(EUnrealAIErrorCategory::InvalidArgument,
												EUnrealAIProviderAccessErrorCode::InvalidSecretStoreContext);
			return false;
		}
		State->Operations.Add(RequestId.Value, Operation);
	}
	(void)Async(EAsyncExecution::ThreadPool, [Operation]() { Operation->Run(); });
	(void)Async(EAsyncExecution::ThreadPool, [Operation]() { Operation->Monitor(); });
	OutHandle = MoveTemp(Operation);
	return true;
}

void FUnrealAIApiKeyProvisioner::BeginShutdown()
{
	State->BeginShutdown();
}

bool FUnrealAIApiKeyProvisioner::IsShutdown() const
{
	FScopeLock Lock(&State->Mutex);
	return State->bShuttingDown;
}

int32 FUnrealAIApiKeyProvisioner::GetActiveOperationCount() const
{
	FScopeLock Lock(&State->Mutex);
	return State->Operations.Num();
}
