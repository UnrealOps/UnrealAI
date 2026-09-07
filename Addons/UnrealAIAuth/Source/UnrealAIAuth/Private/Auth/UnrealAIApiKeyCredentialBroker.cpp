// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIApiKeyCredentialBroker.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#include <atomic>

namespace
{
using namespace UE::UnrealAI::Private;

constexpr double MonitorPollSeconds = 0.001;

bool IsStableBindingName(const FName Name)
{
	if (Name.IsNone())
	{
		return false;
	}
	const FString Text = Name.ToString();
	if (Text.IsEmpty() || Text.Len() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes)
	{
		return false;
	}
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Character = Text[Index];
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9'));
		if ((!bAlphaNumeric && Character != TEXT('.') && Character != TEXT('_') && Character != TEXT('-')) ||
															 ((Index == 0 || Index == Text.Len() - 1) &&
															  !bAlphaNumeric))
		{
			return false;
		}
	}
	return true;
}

FUnrealAIProviderAccessError MakeAccessError(const EUnrealAIErrorCategory Category,
											 const EUnrealAIProviderAccessErrorCode Code, const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

FUnrealAICredentialResult MakeTerminalResult(const FUnrealAIRequestId RequestId,
											 const EUnrealAICredentialResultKind Kind)
{
	FUnrealAICredentialResult Result;
	Result.RequestId = RequestId;
	Result.Kind = Kind;
	if (Kind == EUnrealAICredentialResultKind::Cancelled)
	{
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::CredentialCancelled);
	}
	else if (Kind == EUnrealAICredentialResultKind::TimedOut)
	{
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::CredentialTimedOut);
	}
	else if (Kind == EUnrealAICredentialResultKind::Failed)
	{
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::CredentialFailed);
	}
	return Result;
}

FUnrealAICredentialResult MakeStoreTerminalResult(const FUnrealAIRequestId RequestId,
												  const EUnrealAISecretStoreResult StoreResult,
												  const FUnrealAIProviderAccessError &StoreError)
{
	FUnrealAICredentialResult Result;
	Result.RequestId = RequestId;
	Result.Kind = EUnrealAICredentialResultKind::Failed;
	switch (StoreResult)
	{
	case EUnrealAISecretStoreResult::NotFound:
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::NotFound, EUnrealAIProviderAccessErrorCode::SecretNotFound);
		break;
	case EUnrealAISecretStoreResult::Conflict:
		Result.Error = MakeAccessError(EUnrealAIErrorCategory::VersionMismatch,
									   EUnrealAIProviderAccessErrorCode::SecretRevisionConflict);
		break;
	case EUnrealAISecretStoreResult::Locked:
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::SecretStoreLocked, true);
		break;
	case EUnrealAISecretStoreResult::Denied:
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::PolicyDenied, EUnrealAIProviderAccessErrorCode::SecretStoreDenied);
		break;
	case EUnrealAISecretStoreResult::Unavailable:
		Result.Error = MakeAccessError(EUnrealAIErrorCategory::Persistence,
									   EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, true);
		break;
	case EUnrealAISecretStoreResult::NotSupported:
		Result.Error = MakeAccessError(EUnrealAIErrorCategory::UnsupportedCapability,
									   EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
		break;
	case EUnrealAISecretStoreResult::Corrupt:
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
		break;
	case EUnrealAISecretStoreResult::Cancelled:
		Result.Kind = EUnrealAICredentialResultKind::Cancelled;
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
		break;
	case EUnrealAISecretStoreResult::TimedOut:
		Result.Kind = EUnrealAICredentialResultKind::TimedOut;
		Result.Error = MakeAccessError(EUnrealAIErrorCategory::Timeout,
									   EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut, true);
		break;
	case EUnrealAISecretStoreResult::Failed:
	{
		FString ErrorShape;
		Result.Error =
			StoreError.IsError() && StoreError.ValidateShape(ErrorShape)
				? StoreError
				: MakeAccessError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		break;
	}
	case EUnrealAISecretStoreResult::Succeeded:
	default:
		Result.Error =
			MakeAccessError(EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt);
		break;
	}
	return Result;
}

EUnrealAITerminalKind TerminalKindForCredentialResult(const EUnrealAICredentialResultKind Kind)
{
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

struct FApiKeyBindingKey final
{
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;

	friend bool operator==(const FApiKeyBindingKey &A, const FApiKeyBindingKey &B)
	{
		return A.AuthProfileId == B.AuthProfileId && A.AccountId == B.AccountId;
	}
	friend uint32 GetTypeHash(const FApiKeyBindingKey &Key)
	{
		return HashCombine(GetTypeHash(Key.AuthProfileId), GetTypeHash(Key.AccountId));
	}
};

struct FApiKeyEpochSnapshot final
{
	uint64 AuthProfile = 0;
	uint64 Account = 0;
	uint64 Connection = 0;
};

struct FApiKeyBindingRecord final
{
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FUnrealAISecretHandle SecretHandle;
	uint64 ConnectionRevision = 0;
	uint64 EndpointPolicyRevision = 0;
};

enum class EApiKeyAccessCreationResult : uint8
{
	Succeeded,
	Invalidated,
	Capacity,
	Failed
};
} // namespace

namespace UE::UnrealAI::Private
{
class FApiKeyCredentialOperationState final
{
  public:
	FApiKeyCredentialOperationState(const FUnrealAICredentialRequest &Request,
									TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> InConnection,
									const FUnrealAISecretHandle &InSecretHandle,
									TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
									const FUnrealAICancellationToken &ParentCancellation,
									TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> InSink,
									const FApiKeyEpochSnapshot &InEpochs)
		: RequestId(Request.RequestId), Connection(MoveTemp(InConnection)), SecretHandle(InSecretHandle),
		  Clock(MoveTemp(InClock)), Cancellation(ParentCancellation),
		  RequestDeadline(FUnrealAIDeadline::FromNow(*Clock, static_cast<double>(Request.TimeoutSeconds))),
		  Sink(InSink.ToSharedPtr()), Epochs(InEpochs)
	{
		FString ContextError;
		const double StoreTimeout = FMath::Min(static_cast<double>(Request.TimeoutSeconds),
											   FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds);
		bHasStoreContext = FUnrealAISecretStoreOperationContext::TryCreate(Clock, StoreTimeout, Cancellation.GetToken(),
																		   StoreContext, ContextError);
	}

	FUnrealAIRequestId GetRequestId() const
	{
		return RequestId;
	}

	const FUnrealAIConnectionDescriptor &GetConnection() const
	{
		return Connection.Get();
	}

	const FUnrealAISecretHandle &GetSecretHandle() const
	{
		return SecretHandle;
	}

	const FUnrealAISecretStoreOperationContext &GetStoreContext() const
	{
		return StoreContext;
	}

	bool HasStoreContext() const
	{
		return bHasStoreContext;
	}

	const FApiKeyEpochSnapshot &GetEpochs() const
	{
		return Epochs;
	}

	bool IsComplete() const
	{
		return Terminal.IsComplete();
	}

	bool TryCancel(const EUnrealAICancellationReason Reason)
	{
		Cancellation.Cancel(Reason);
		return TryPublish(MakeTerminalResult(RequestId, EUnrealAICredentialResultKind::Cancelled),
						  EUnrealAITerminalKind::Cancelled);
	}

	bool TryTimeout()
	{
		Cancellation.Cancel(EUnrealAICancellationReason::Timeout);
		return TryPublish(MakeTerminalResult(RequestId, EUnrealAICredentialResultKind::TimedOut),
						  EUnrealAITerminalKind::TimedOut);
	}

	bool TryFail()
	{
		return TryPublish(MakeTerminalResult(RequestId, EUnrealAICredentialResultKind::Failed),
						  EUnrealAITerminalKind::Failed);
	}

	bool TryResourceCapacity()
	{
		FUnrealAICredentialResult Result;
		Result.RequestId = RequestId;
		Result.Kind = EUnrealAICredentialResultKind::Failed;
		Result.Error = MakeAccessError(EUnrealAIErrorCategory::Busy,
									   EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity, true);
		return TryPublish(MoveTemp(Result), EUnrealAITerminalKind::Failed);
	}

	bool TryStoreTerminal(const EUnrealAISecretStoreResult StoreResult, const FUnrealAIProviderAccessError &StoreError)
	{
		FUnrealAICredentialResult Result = MakeStoreTerminalResult(RequestId, StoreResult, StoreError);
		const EUnrealAITerminalKind TerminalKind = TerminalKindForCredentialResult(Result.Kind);
		return TryPublish(MoveTemp(Result), TerminalKind);
	}

	bool TrySucceed(TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext)
	{
		FUnrealAICredentialResult Result;
		Result.RequestId = RequestId;
		Result.Kind = EUnrealAICredentialResultKind::Succeeded;
		Result.AccessContext = MoveTemp(AccessContext);
		return TryPublish(MoveTemp(Result), EUnrealAITerminalKind::Succeeded);
	}

	void ObserveCancellationAndTimeout()
	{
		if (Terminal.IsComplete())
		{
			return;
		}
		const FUnrealAICancellationToken Token = Cancellation.GetToken();
		if (Token.IsCancellationRequested())
		{
			if (Token.GetReason() == EUnrealAICancellationReason::Timeout)
			{
				TryTimeout();
			}
			else
			{
				TryCancel(Token.GetReason());
			}
			return;
		}
		if (RequestDeadline.IsExpired(*Clock))
		{
			TryTimeout();
		}
	}

	void Monitor()
	{
		while (!Terminal.IsComplete())
		{
			ObserveCancellationAndTimeout();
			if (!Terminal.IsComplete())
			{
				FPlatformProcess::SleepNoStats(static_cast<float>(MonitorPollSeconds));
			}
		}
	}

  private:
	bool TryPublish(FUnrealAICredentialResult &&Result, const EUnrealAITerminalKind TerminalKind)
	{
		if (!Terminal.TryComplete(TerminalKind))
		{
			return false;
		}

		FString ShapeError;
		if (!Result.ValidateShape(ShapeError))
		{
			Result = MakeTerminalResult(RequestId, EUnrealAICredentialResultKind::Failed);
		}

		TSharedPtr<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> LocalSink;
		{
			FScopeLock Lock(&SinkMutex);
			LocalSink = MoveTemp(Sink);
		}
		if (LocalSink.IsValid())
		{
			LocalSink->EnqueueCredentialResult(MoveTemp(Result));
		}
		return true;
	}

	FUnrealAIRequestId RequestId;
	TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection;
	FUnrealAISecretHandle SecretHandle;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAICancellationSource Cancellation;
	FUnrealAIDeadline RequestDeadline;
	FUnrealAISecretStoreOperationContext StoreContext;
	bool bHasStoreContext = false;
	mutable FCriticalSection SinkMutex;
	TSharedPtr<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink;
	FApiKeyEpochSnapshot Epochs;
	FUnrealAITerminalGuard Terminal;
};

class FApiKeyCredentialRequestHandle final : public IUnrealAICredentialRequestHandle
{
  public:
	explicit FApiKeyCredentialRequestHandle(
		TSharedRef<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> InOperation)
		: Operation(MoveTemp(InOperation))
	{
	}

	FUnrealAIRequestId GetRequestId() const override
	{
		return Operation->GetRequestId();
	}

	void Cancel() override
	{
		Operation->TryCancel(EUnrealAICancellationReason::Requested);
	}

  private:
	TSharedRef<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> Operation;
};

class FApiKeyCredentialBrokerState final : public TSharedFromThis<FApiKeyCredentialBrokerState, ESPMode::ThreadSafe>
{
  public:
	struct FActiveLease final
	{
		FName ConnectionAlias;
		FName AuthProfileId;
		FUnrealAIAccessAccountId AccountId;
		double ExpiresAtMonotonicSeconds = 0.0;
		TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Consumed;
		TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> Freshness;
	};

	FApiKeyCredentialBrokerState(
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		const FUnrealAIApiKeyCredentialBrokerConfig &InConfig)
		: Connections(MoveTemp(InConnections)), SecretStore(MoveTemp(InSecretStore)), Clock(MoveTemp(InClock)),
		  Config(InConfig)
	{
		FString ConfigError;
		if (!Config.ValidateShape(ConfigError))
		{
			bShutdown = true;
		}
	}

	bool RegisterBinding(const FUnrealAIApiKeyCredentialBinding &Binding, FString &OutError)
	{
		OutError.Reset();
		if (!Binding.ValidateShape(OutError) || Binding.SecretHandle.StoreName != SecretStore->GetStoreName())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("API-key binding rejected a handle owned by a different secure store.");
			}
			return false;
		}
		const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection =
			Connections->Find(Binding.ConnectionAlias);
		if (!Connection.IsValid())
		{
			OutError = TEXT("API-key binding rejected an unknown exact connection alias.");
			return false;
		}
		const FUnrealAICredentialDestination &Destination = Connection->CredentialDestination;
		if (Destination.AuthScheme != EUnrealAIAuthScheme::ApiKey ||
			Destination.BillingMode != EUnrealAIBillingMode::ApiMetered ||
			Destination.AuthProfileId != Binding.AuthProfileId || Destination.AccountId != Binding.AccountId)
		{
			OutError = TEXT("API-key binding rejected connection, auth-profile, account, credential-scheme, or billing drift.");
			return false;
		}
		const FApiKeyBindingKey AccountKey{Binding.AuthProfileId, Binding.AccountId};
		FApiKeyBindingRecord Record;
		Record.AuthProfileId = Binding.AuthProfileId;
		Record.AccountId = Binding.AccountId;
		Record.SecretHandle = Binding.SecretHandle;
		Record.ConnectionRevision = Connection->ConnectionRevision;
		Record.EndpointPolicyRevision = Destination.EndpointPolicyRevision;
		FScopeLock Lock(&Mutex);
		if (bShutdown || DisabledAccounts.Contains(AccountKey) || Bindings.Contains(Binding.ConnectionAlias) ||
			Bindings.Num() >= FUnrealAIConnectionRegistry::MaxConnections)
		{
			OutError = bShutdown ? TEXT("API-key broker is shut down.")
								 : (DisabledAccounts.Contains(AccountKey)
									? TEXT("API-key broker rejected a quarantined profile/account binding.")
									: (Bindings.Contains(Binding.ConnectionAlias)
									   ? TEXT("API-key broker rejected a duplicate exact connection binding.")
									   : TEXT("API-key broker binding registry reached capacity.")));
			return false;
		}
		Bindings.Add(Binding.ConnectionAlias, MoveTemp(Record));
		return true;
	}

	bool StartResolve(const FUnrealAICredentialRequest &Request,
					  TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError)
	{
		OutHandle.Reset();
		OutError = FUnrealAIProviderAccessError{};
		FString ShapeError;
		if (!Request.ValidateShape(ShapeError) || !Cancellation.IsValid())
		{
			OutError = MakeAccessError(EUnrealAIErrorCategory::InvalidArgument,
									   EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError = MakeAccessError(EUnrealAIErrorCategory::Cancelled,
									   EUnrealAIProviderAccessErrorCode::CredentialCancelled);
			return false;
		}

		const TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Found =
			Connections->Find(Request.ConnectionAlias);
		if (!Found.IsValid())
		{
			OutError = MakeAccessError(EUnrealAIErrorCategory::NotAuthorized,
									   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
			return false;
		}
		const TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection = Found.ToSharedRef();
		const FUnrealAICredentialDestination &Destination = Connection->CredentialDestination;
		if (Destination.AuthScheme != EUnrealAIAuthScheme::ApiKey ||
			Destination.BillingMode != EUnrealAIBillingMode::ApiMetered)
		{
			OutError = MakeAccessError(EUnrealAIErrorCategory::UnsupportedCapability,
									   EUnrealAIProviderAccessErrorCode::UnsupportedCapability);
			return false;
		}

		const FApiKeyBindingKey BindingKey{Destination.AuthProfileId, Destination.AccountId};
		FUnrealAISecretHandle SecretHandle;
		FApiKeyEpochSnapshot Epochs;
		{
			FScopeLock Lock(&Mutex);
			const FApiKeyBindingRecord *FoundBinding = Bindings.Find(Connection->ConnectionAlias);
			const bool bBindingMatches = FoundBinding != nullptr &&
										 FoundBinding->AuthProfileId == Destination.AuthProfileId &&
										 FoundBinding->AccountId == Destination.AccountId &&
										 FoundBinding->ConnectionRevision == Connection->ConnectionRevision &&
										 FoundBinding->EndpointPolicyRevision == Destination.EndpointPolicyRevision;
			if (bShutdown || DisabledAccounts.Contains(BindingKey) || !bBindingMatches)
			{
				OutError = MakeAccessError(EUnrealAIErrorCategory::NotAuthorized,
										   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
				return false;
			}
			SecretHandle = FoundBinding->SecretHandle;
			Epochs = FApiKeyEpochSnapshot{AuthProfileEpochs.FindRef(Destination.AuthProfileId),
										  AccountEpochs.FindRef(BindingKey),
										  ConnectionEpochs.FindRef(Connection->ConnectionAlias)};
		}

		const TSharedRef<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> Operation =
			MakeShared<FApiKeyCredentialOperationState, ESPMode::ThreadSafe>(Request, Connection, SecretHandle, Clock,
																			 Cancellation, Sink, Epochs);
		if (!Operation->HasStoreContext())
		{
			OutError = MakeAccessError(EUnrealAIErrorCategory::InvalidArgument,
									   EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}

		{
			FScopeLock Lock(&Mutex);
			const FApiKeyBindingRecord *CurrentBinding = Bindings.Find(Connection->ConnectionAlias);
			const bool bEpochsCurrent = AuthProfileEpochs.FindRef(Destination.AuthProfileId) == Epochs.AuthProfile &&
										AccountEpochs.FindRef(BindingKey) == Epochs.Account &&
										ConnectionEpochs.FindRef(Connection->ConnectionAlias) == Epochs.Connection;
			const bool bBindingCurrent =
				CurrentBinding != nullptr && CurrentBinding->AuthProfileId == Destination.AuthProfileId &&
				CurrentBinding->AccountId == Destination.AccountId && CurrentBinding->SecretHandle == SecretHandle &&
				CurrentBinding->ConnectionRevision == Connection->ConnectionRevision &&
				CurrentBinding->EndpointPolicyRevision == Destination.EndpointPolicyRevision;
			if (bShutdown || DisabledAccounts.Contains(BindingKey) || !bBindingCurrent || !bEpochsCurrent)
			{
				OutError = MakeAccessError(EUnrealAIErrorCategory::NotAuthorized,
										   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
				return false;
			}
			if (ActiveRequestIds.Contains(Request.RequestId))
			{
				OutError = MakeAccessError(EUnrealAIErrorCategory::InvalidArgument,
										   EUnrealAIProviderAccessErrorCode::InvalidRequest);
				return false;
			}
			if (PhysicalStoreOperations >= Config.MaxConcurrentStoreOperations)
			{
				OutError = MakeAccessError(EUnrealAIErrorCategory::Busy,
										   EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity, true);
				return false;
			}
			++PhysicalStoreOperations;
			ActiveRequestIds.Add(Request.RequestId);
			Operations.Add(Operation);
		}

		OutHandle = MakeShared<FApiKeyCredentialRequestHandle, ESPMode::ThreadSafe>(Operation);
		const TSharedRef<FApiKeyCredentialBrokerState, ESPMode::ThreadSafe> Self = AsShared();
		(void)Async(EAsyncExecution::Thread, [Self, Operation]() { Self->RunPhysicalStoreLoad(Operation); });
		(void)Async(EAsyncExecution::Thread, [Operation]() { Operation->Monitor(); });
		return true;
	}

	void InvalidateAccount(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId)
	{
		const FApiKeyBindingKey Key{AuthProfileId, AccountId};
		InvalidateMatching(
			[&Key](const FUnrealAIConnectionDescriptor &Connection)
			{
				return Connection.CredentialDestination.AuthProfileId == Key.AuthProfileId &&
					   Connection.CredentialDestination.AccountId == Key.AccountId;
			},
			[&Key](const FActiveLease &Lease)
			{ return Lease.AuthProfileId == Key.AuthProfileId && Lease.AccountId == Key.AccountId; },
			[this, &Key]() { BumpEpoch(AccountEpochs.FindOrAdd(Key)); });
	}

	void QuarantineAccount(const FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId)
	{
		const FApiKeyBindingKey Key{AuthProfileId, AccountId};
		InvalidateMatching(
			[&Key](const FUnrealAIConnectionDescriptor &Connection)
			{
				return Connection.CredentialDestination.AuthProfileId == Key.AuthProfileId &&
					   Connection.CredentialDestination.AccountId == Key.AccountId;
			},
			[&Key](const FActiveLease &Lease)
			{ return Lease.AuthProfileId == Key.AuthProfileId && Lease.AccountId == Key.AccountId; },
			[this, &Key]()
			{
				DisabledAccounts.Add(Key);
				BumpEpoch(AccountEpochs.FindOrAdd(Key));
			});
	}

	void InvalidateAuthProfile(const FName AuthProfileId)
	{
		InvalidateMatching([AuthProfileId](const FUnrealAIConnectionDescriptor &Connection)
						   { return Connection.CredentialDestination.AuthProfileId == AuthProfileId; },
						   [AuthProfileId](const FActiveLease &Lease) { return Lease.AuthProfileId == AuthProfileId; },
						   [this, AuthProfileId]() { BumpEpoch(AuthProfileEpochs.FindOrAdd(AuthProfileId)); });
	}

	void InvalidateConnection(const FName ConnectionAlias)
	{
		InvalidateMatching([ConnectionAlias](const FUnrealAIConnectionDescriptor &Connection)
						   { return Connection.ConnectionAlias == ConnectionAlias; },
						   [ConnectionAlias](const FActiveLease &Lease)
						   { return Lease.ConnectionAlias == ConnectionAlias; },
						   [this, ConnectionAlias]() { BumpEpoch(ConnectionEpochs.FindOrAdd(ConnectionAlias)); });
	}

	void BeginShutdown()
	{
		TArray<TSharedPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe>> Pending;
		TArray<TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> Freshness;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown)
			{
				return;
			}
			bShutdown = true;
			for (const TWeakPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> &Weak : Operations)
			{
				if (TSharedPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
				{
					Pending.Add(MoveTemp(Pinned));
				}
			}
			for (const FActiveLease &Lease : ActiveLeases)
			{
				Freshness.Add(Lease.Freshness);
			}
			ActiveLeases.Reset();
		}
		for (const TSharedPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> &Operation : Pending)
		{
			Operation->TryCancel(EUnrealAICancellationReason::Shutdown);
		}
		for (const TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> &Source : Freshness)
		{
			Source->BeginShutdown();
		}
	}

	bool IsShutdown() const
	{
		FScopeLock Lock(&Mutex);
		return bShutdown;
	}

	int32 GetPhysicalStoreOperationCount() const
	{
		FScopeLock Lock(&Mutex);
		return PhysicalStoreOperations;
	}

	int32 GetTrackedOperationCount() const
	{
		FScopeLock Lock(&Mutex);
		return Operations.Num();
	}

	int32 GetActiveLeaseCount() const
	{
		const double Now = Clock->MonotonicSeconds();
		TArray<TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> Retired;
		int32 Count = 0;
		{
			FScopeLock Lock(&Mutex);
			CollectInactiveLeasesLocked(Now, Retired);
			Count = ActiveLeases.Num();
		}
		for (const TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> &Source : Retired)
		{
			Source->BeginShutdown();
		}
		return Count;
	}

	int32 GetDiscardedLateSecretCount() const
	{
		FScopeLock Lock(&Mutex);
		return DiscardedLateSecretCount;
	}

  private:
	template <typename OperationPredicate, typename LeasePredicate, typename EpochMutation>
	void InvalidateMatching(OperationPredicate &&MatchesOperation, LeasePredicate &&MatchesLease,
							EpochMutation &&MutateEpoch)
	{
		TArray<TSharedPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe>> Pending;
		TArray<TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> Freshness;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown)
			{
				return;
			}
			MutateEpoch();
			for (const TWeakPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> &Weak : Operations)
			{
				if (TSharedPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> Pinned = Weak.Pin();
					Pinned.IsValid() && MatchesOperation(Pinned->GetConnection()))
				{
					Pending.Add(MoveTemp(Pinned));
				}
			}
			for (int32 Index = ActiveLeases.Num() - 1; Index >= 0; --Index)
			{
				if (MatchesLease(ActiveLeases[Index]))
				{
					Freshness.Add(ActiveLeases[Index].Freshness);
					ActiveLeases.RemoveAtSwap(Index, 1, EAllowShrinking::No);
				}
			}
		}
		for (const TSharedPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> &Operation : Pending)
		{
			Operation->TryCancel(EUnrealAICancellationReason::Superseded);
		}
		for (const TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> &Source : Freshness)
		{
			Source->Invalidate();
		}
	}

	void RunPhysicalStoreLoad(const TSharedRef<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> &Operation)
	{
		Operation->ObserveCancellationAndTimeout();
		if (!Operation->IsComplete())
		{
			FUnrealAISecretValue Secret;
			uint64 Revision = 0;
			FUnrealAIProviderAccessError StoreError;
			const EUnrealAISecretStoreResult StoreResult = SecretStore->Load(
				Operation->GetStoreContext(), Operation->GetSecretHandle(), Secret, Revision, StoreError);
			Operation->ObserveCancellationAndTimeout();
			if (!Operation->IsComplete())
			{
				if (StoreResult == EUnrealAISecretStoreResult::Succeeded && Secret.IsSet() && Revision != 0)
				{
					TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext;
					TSharedPtr<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> Freshness;
					const EApiKeyAccessCreationResult AccessResult =
						TryCreateAccessContext(Operation, MoveTemp(Secret), AccessContext, Freshness);
					if (AccessResult == EApiKeyAccessCreationResult::Succeeded)
					{
						if (!Operation->TrySucceed(MoveTemp(AccessContext)) && Freshness.IsValid())
						{
							DiscardFreshness(Freshness.ToSharedRef());
						}
					}
					else if (AccessResult == EApiKeyAccessCreationResult::Invalidated)
					{
						Operation->TryCancel(EUnrealAICancellationReason::Superseded);
					}
					else if (AccessResult == EApiKeyAccessCreationResult::Capacity)
					{
						Operation->TryResourceCapacity();
					}
					else
					{
						Operation->TryFail();
					}
				}
				else
				{
					Operation->TryStoreTerminal(StoreResult, StoreError);
				}
			}
			else if (Secret.IsSet())
			{
				Secret.Reset();
				FScopeLock Lock(&Mutex);
				++DiscardedLateSecretCount;
			}
		}
		OnPhysicalStoreOperationFinished(Operation);
	}

	EApiKeyAccessCreationResult
	TryCreateAccessContext(const TSharedRef<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> &Operation,
						   FUnrealAISecretValue &&Secret,
						   TSharedPtr<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> &OutContext,
						   TSharedPtr<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> &OutFreshness)
	{
		OutContext.Reset();
		OutFreshness.Reset();
		const double Now = Clock->MonotonicSeconds();
		if (!FMath::IsFinite(Now) || Now < 0.0)
		{
			Secret.Reset();
			return EApiKeyAccessCreationResult::Failed;
		}
		TArray<TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> Retired;
		const TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> Freshness =
			MakeShared<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>();
		const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Consumed =
			MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		EApiKeyAccessCreationResult AdmissionResult = EApiKeyAccessCreationResult::Succeeded;
		{
			FScopeLock Lock(&Mutex);
			CollectInactiveLeasesLocked(Now, Retired);
			const FUnrealAIConnectionDescriptor &Connection = Operation->GetConnection();
			const FUnrealAICredentialDestination &Destination = Connection.CredentialDestination;
			const FApiKeyBindingKey AccountKey{Destination.AuthProfileId, Destination.AccountId};
			const FApiKeyEpochSnapshot &Epochs = Operation->GetEpochs();
			if (bShutdown || DisabledAccounts.Contains(AccountKey) ||
				AuthProfileEpochs.FindRef(Destination.AuthProfileId) != Epochs.AuthProfile ||
				AccountEpochs.FindRef(AccountKey) != Epochs.Account ||
				ConnectionEpochs.FindRef(Connection.ConnectionAlias) != Epochs.Connection)
			{
				AdmissionResult = EApiKeyAccessCreationResult::Invalidated;
			}
			else if (ActiveLeases.Num() >= Config.MaxActiveLeases)
			{
				AdmissionResult = EApiKeyAccessCreationResult::Capacity;
			}
			else
			{
				FActiveLease Lease{Connection.ConnectionAlias,
								   Destination.AuthProfileId,
								   Destination.AccountId,
								   Now + Config.LeaseLifetimeSeconds,
								   Consumed,
								   Freshness};
				ActiveLeases.Add(MoveTemp(Lease));
			}
		}
		for (const TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> &Source : Retired)
		{
			Source->BeginShutdown();
		}
		if (AdmissionResult != EApiKeyAccessCreationResult::Succeeded)
		{
			Secret.Reset();
			return AdmissionResult;
		}

		FUnrealAICredentialLease Lease;
		FString LeaseError;
		if (!FUnrealAICredentialLease::TryCreate(Operation->GetConnection().CredentialDestination, Clock,
												 Freshness->GetToken(), Config.LeaseLifetimeSeconds, {},
												 MoveTemp(Secret), Lease, LeaseError,
												 [Consumed]() { Consumed->store(true, std::memory_order_release); }))
		{
			DiscardFreshness(Freshness);
			return EApiKeyAccessCreationResult::Invalidated;
		}
		OutContext = IUnrealAIProviderAccessContext::CreateCredentialed(MoveTemp(Lease));
		if (!OutContext.IsValid() || !OutContext->IsValid())
		{
			OutContext.Reset();
			DiscardFreshness(Freshness);
			return EApiKeyAccessCreationResult::Invalidated;
		}
		OutFreshness = Freshness;
		return EApiKeyAccessCreationResult::Succeeded;
	}

	void DiscardFreshness(const TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe> &Freshness)
	{
		{
			FScopeLock Lock(&Mutex);
			for (int32 Index = ActiveLeases.Num() - 1; Index >= 0; --Index)
			{
				if (&ActiveLeases[Index].Freshness.Get() == &Freshness.Get())
				{
					ActiveLeases.RemoveAtSwap(Index, 1, EAllowShrinking::No);
				}
			}
		}
		Freshness->BeginShutdown();
	}

	void CollectInactiveLeasesLocked(
		const double Now, TArray<TSharedRef<FUnrealAICredentialFreshnessSource, ESPMode::ThreadSafe>> &OutRetired) const
	{
		for (int32 Index = ActiveLeases.Num() - 1; Index >= 0; --Index)
		{
			const FActiveLease &Lease = ActiveLeases[Index];
			if (Lease.ExpiresAtMonotonicSeconds <= Now || Lease.Consumed->load(std::memory_order_acquire))
			{
				OutRetired.Add(Lease.Freshness);
				ActiveLeases.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
	}

	void OnPhysicalStoreOperationFinished(
		const TSharedRef<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> &CompletedOperation)
	{
		FScopeLock Lock(&Mutex);
		PhysicalStoreOperations = FMath::Max(0, PhysicalStoreOperations - 1);
		ActiveRequestIds.Remove(CompletedOperation->GetRequestId());
		Operations.RemoveAllSwap(
			[&CompletedOperation](const TWeakPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> &Weak)
			{
				const TSharedPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe> Operation = Weak.Pin();
				return !Operation.IsValid() || Operation.Get() == &CompletedOperation.Get();
			},
			EAllowShrinking::No);
	}

	void BumpEpoch(uint64 &Epoch)
	{
		if (Epoch == TNumericLimits<uint64>::Max())
		{
			bShutdown = true;
		}
		else
		{
			++Epoch;
		}
	}

	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAIApiKeyCredentialBrokerConfig Config;
	mutable FCriticalSection Mutex;
	TMap<FName, FApiKeyBindingRecord> Bindings;
	TMap<FName, uint64> AuthProfileEpochs;
	TMap<FApiKeyBindingKey, uint64> AccountEpochs;
	TMap<FName, uint64> ConnectionEpochs;
	TSet<FApiKeyBindingKey> DisabledAccounts;
	TArray<TWeakPtr<FApiKeyCredentialOperationState, ESPMode::ThreadSafe>> Operations;
	mutable TArray<FActiveLease> ActiveLeases;
	TSet<FUnrealAIRequestId> ActiveRequestIds;
	int32 PhysicalStoreOperations = 0;
	int32 DiscardedLateSecretCount = 0;
	bool bShutdown = false;
};
} // namespace UE::UnrealAI::Private

FUnrealAIApiKeyCredentialBroker::FUnrealAIApiKeyCredentialBroker(
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	const FUnrealAIApiKeyCredentialBrokerConfig &InConfig)
	: State(MakeShared<UE::UnrealAI::Private::FApiKeyCredentialBrokerState, ESPMode::ThreadSafe>(
		  MoveTemp(InConnections), MoveTemp(InSecretStore), MoveTemp(InClock), InConfig))
{
}

FUnrealAIApiKeyCredentialBroker::~FUnrealAIApiKeyCredentialBroker()
{
	BeginShutdown();
}

bool FUnrealAIApiKeyCredentialBroker::RegisterBinding(const FUnrealAIApiKeyCredentialBinding &Binding,
													  FString &OutError)
{
	return State->RegisterBinding(Binding, OutError);
}

bool FUnrealAIApiKeyCredentialBroker::StartResolve(
	const FUnrealAICredentialRequest &Request, TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink,
	const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
	FUnrealAIProviderAccessError &OutError)
{
	return State->StartResolve(Request, MoveTemp(Sink), Cancellation, OutHandle, OutError);
}

void FUnrealAIApiKeyCredentialBroker::InvalidateAccount(const FName AuthProfileId,
														const FUnrealAIAccessAccountId &AccountId)
{
	State->InvalidateAccount(AuthProfileId, AccountId);
}

void FUnrealAIApiKeyCredentialBroker::QuarantineAccount(const FName AuthProfileId,
														const FUnrealAIAccessAccountId &AccountId)
{
	State->QuarantineAccount(AuthProfileId, AccountId);
}

void FUnrealAIApiKeyCredentialBroker::InvalidateAuthProfile(const FName AuthProfileId)
{
	State->InvalidateAuthProfile(AuthProfileId);
}

void FUnrealAIApiKeyCredentialBroker::InvalidateConnection(const FName ConnectionAlias)
{
	State->InvalidateConnection(ConnectionAlias);
}

void FUnrealAIApiKeyCredentialBroker::BeginShutdown()
{
	State->BeginShutdown();
}

bool FUnrealAIApiKeyCredentialBroker::IsShutdown() const
{
	return State->IsShutdown();
}

int32 FUnrealAIApiKeyCredentialBroker::GetPhysicalStoreOperationCount() const
{
	return State->GetPhysicalStoreOperationCount();
}

int32 FUnrealAIApiKeyCredentialBroker::GetTrackedOperationCount() const
{
	return State->GetTrackedOperationCount();
}

int32 FUnrealAIApiKeyCredentialBroker::GetActiveLeaseCount() const
{
	return State->GetActiveLeaseCount();
}

int32 FUnrealAIApiKeyCredentialBroker::GetDiscardedLateSecretCount() const
{
	return State->GetDiscardedLateSecretCount();
}
