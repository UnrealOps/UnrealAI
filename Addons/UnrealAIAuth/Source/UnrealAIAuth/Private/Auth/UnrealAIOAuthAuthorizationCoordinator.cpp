// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthAuthorizationCoordinator.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace
{
FUnrealAIProviderAccessError MakeOAuthCoordinatorError(const EUnrealAIErrorCategory Category,
													   const EUnrealAIProviderAccessErrorCode Code,
													   const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

FUnrealAIProviderAccessError NormalizeOAuthCoordinatorError(const FUnrealAIProviderAccessError &Candidate)
{
	FString ShapeError;
	if (!Candidate.IsError() || !Candidate.ValidateShape(ShapeError))
	{
		return MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Provider,
										 EUnrealAIProviderAccessErrorCode::AuthFailed);
	}
	if (Candidate.Category == EUnrealAIErrorCategory::Cancelled ||
		Candidate.Code == EUnrealAIProviderAccessErrorCode::AuthCancelled)
	{
		return MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Cancelled,
										 EUnrealAIProviderAccessErrorCode::AuthCancelled);
	}
	if (Candidate.Category == EUnrealAIErrorCategory::Timeout ||
		Candidate.Code == EUnrealAIProviderAccessErrorCode::AuthTimedOut)
	{
		return MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Timeout,
										 EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
	}
	return Candidate;
}

bool IsEmptyAuthorizationResult(const FUnrealAIOAuthAuthorizationResult &Result)
{
	return !Result.Tokens.AccessToken.IsSet() && !Result.Tokens.RefreshToken.IsSet() &&
		   !Result.Tokens.IdToken.IsSet() && Result.SubjectFingerprint.IsEmpty() && Result.GrantedScopes.IsEmpty();
}
} // namespace

namespace UE::UnrealAI::Private
{
class FOAuthAuthorizationOperation final : public IUnrealAIOAuthAuthorizationOperationHandle,
										   public TSharedFromThis<FOAuthAuthorizationOperation, ESPMode::ThreadSafe>
{
  public:
	FOAuthAuthorizationOperation(
		const FUnrealAIOAuthBrowserAuthorizationRequest &InRequest,
		TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> InCompletionSink,
		const FUnrealAICancellationToken &ParentCancellation, const double InDeadlinePollSeconds,
		TFunction<void(const FUnrealAIRequestId &)> InOnTerminal,
		TFunction<void(const FUnrealAIRequestId &)> InOnPhysical)
		: RequestId(InRequest.RequestId), OperationKind(EUnrealAIOAuthAuthorizationOperationKind::AuthorizeBrowserPkce),
		  BrowserRequest(InRequest), Executor(MoveTemp(InExecutor)), Clock(MoveTemp(InClock)),
		  Cancellation(ParentCancellation), Deadline(FUnrealAIDeadline::FromNow(*Clock, InRequest.TimeoutSeconds)),
		  DeadlinePollSeconds(InDeadlinePollSeconds), CompletionSink(MoveTemp(InCompletionSink)),
		  OnTerminal(MoveTemp(InOnTerminal)), OnPhysical(MoveTemp(InOnPhysical))
	{
	}

	FOAuthAuthorizationOperation(
		const FUnrealAIOAuthRevocationRequest &InRequest, FUnrealAISecretValue &&InToken,
		TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> InCompletionSink,
		const FUnrealAICancellationToken &ParentCancellation, const double InDeadlinePollSeconds,
		TFunction<void(const FUnrealAIRequestId &)> InOnTerminal,
		TFunction<void(const FUnrealAIRequestId &)> InOnPhysical)
		: RequestId(InRequest.RequestId), OperationKind(EUnrealAIOAuthAuthorizationOperationKind::Revoke),
		  RevocationRequest(InRequest), RevocationToken(MoveTemp(InToken)), Executor(MoveTemp(InExecutor)),
		  Clock(MoveTemp(InClock)), Cancellation(ParentCancellation),
		  Deadline(FUnrealAIDeadline::FromNow(*Clock, InRequest.TimeoutSeconds)),
		  DeadlinePollSeconds(InDeadlinePollSeconds), CompletionSink(MoveTemp(InCompletionSink)),
		  OnTerminal(MoveTemp(InOnTerminal)), OnPhysical(MoveTemp(InOnPhysical))
	{
	}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}

	void Cancel() override
	{
		Cancellation.Cancel(EUnrealAICancellationReason::Requested);
		PublishFailure(MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Cancelled,
												 EUnrealAIProviderAccessErrorCode::AuthCancelled));
	}

	void CancelForShutdown()
	{
		Cancellation.Cancel(EUnrealAICancellationReason::Shutdown);
		PublishFailure(MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Cancelled,
												 EUnrealAIProviderAccessErrorCode::AuthCancelled));
	}

	void Run()
	{
		FUnrealAIOAuthAuthorizationResult Result;
		FUnrealAIProviderAccessError Error;
		bool bSucceeded = false;
		if (OperationKind == EUnrealAIOAuthAuthorizationOperationKind::AuthorizeBrowserPkce)
		{
			bSucceeded = Executor->AuthorizeBrowserPkce(BrowserRequest, Cancellation.GetToken(), Result, Error);
		}
		else if (OperationKind == EUnrealAIOAuthAuthorizationOperationKind::Revoke)
		{
			bSucceeded = Executor->Revoke(RevocationRequest, MoveTemp(RevocationToken), Cancellation.GetToken(), Error);
		}
		else
		{
			Error =
				MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		}

		if (!TryObserveLogicalTerminal())
		{
			if (bSucceeded)
			{
				PublishSuccess(MoveTemp(Result));
			}
			else
			{
				PublishFailure(NormalizeOAuthCoordinatorError(Error));
			}
		}
		Result.Reset();
		RevocationToken.Reset();
		OnPhysical(RequestId);
	}

	void Monitor()
	{
		while (!Terminal.IsComplete())
		{
			TryObserveLogicalTerminal();
			if (!Terminal.IsComplete())
			{
				FPlatformProcess::SleepNoStats(static_cast<float>(DeadlinePollSeconds));
			}
		}
	}

	bool TryObserveLogicalTerminal()
	{
		if (Terminal.IsComplete())
		{
			return false;
		}
		const FUnrealAICancellationToken Token = Cancellation.GetToken();
		if (Token.IsCancellationRequested())
		{
			if (Token.GetReason() == EUnrealAICancellationReason::Timeout)
			{
				return PublishFailure(MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Timeout,
																EUnrealAIProviderAccessErrorCode::AuthTimedOut, true));
			}
			return PublishFailure(MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Cancelled,
															EUnrealAIProviderAccessErrorCode::AuthCancelled));
		}
		if (Deadline.IsExpired(*Clock))
		{
			Cancellation.Cancel(EUnrealAICancellationReason::Timeout);
			return PublishFailure(MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Timeout,
															EUnrealAIProviderAccessErrorCode::AuthTimedOut, true));
		}
		return false;
	}

  private:
	bool PublishSuccess(FUnrealAIOAuthAuthorizationResult &&Result)
	{
		if (!Terminal.TryComplete(EUnrealAITerminalKind::Succeeded))
		{
			Result.Reset();
			return false;
		}
		FUnrealAIOAuthAuthorizationCompletion Completion;
		Completion.RequestId = RequestId;
		Completion.OperationKind = OperationKind;
		Completion.TerminalKind = EUnrealAIOAuthAuthorizationTerminalKind::Succeeded;
		if (OperationKind == EUnrealAIOAuthAuthorizationOperationKind::AuthorizeBrowserPkce)
		{
			Completion.AuthorizationResult = MoveTemp(Result);
		}
		else
		{
			Result.Reset();
		}
		OnTerminal(RequestId);
		CompletionSink->CompleteOAuthAuthorization(MoveTemp(Completion));
		return true;
	}

	bool PublishFailure(const FUnrealAIProviderAccessError &InError)
	{
		const FUnrealAIProviderAccessError Error = NormalizeOAuthCoordinatorError(InError);
		EUnrealAITerminalKind GuardKind = EUnrealAITerminalKind::Failed;
		EUnrealAIOAuthAuthorizationTerminalKind CompletionKind = EUnrealAIOAuthAuthorizationTerminalKind::Failed;
		if (Error.Category == EUnrealAIErrorCategory::Cancelled)
		{
			GuardKind = EUnrealAITerminalKind::Cancelled;
			CompletionKind = EUnrealAIOAuthAuthorizationTerminalKind::Cancelled;
		}
		else if (Error.Category == EUnrealAIErrorCategory::Timeout)
		{
			GuardKind = EUnrealAITerminalKind::TimedOut;
			CompletionKind = EUnrealAIOAuthAuthorizationTerminalKind::TimedOut;
		}
		if (!Terminal.TryComplete(GuardKind))
		{
			return false;
		}
		FUnrealAIOAuthAuthorizationCompletion Completion;
		Completion.RequestId = RequestId;
		Completion.OperationKind = OperationKind;
		Completion.TerminalKind = CompletionKind;
		Completion.Error = Error;
		OnTerminal(RequestId);
		CompletionSink->CompleteOAuthAuthorization(MoveTemp(Completion));
		return true;
	}

	FUnrealAIRequestId RequestId;
	EUnrealAIOAuthAuthorizationOperationKind OperationKind = EUnrealAIOAuthAuthorizationOperationKind::Invalid;
	FUnrealAIOAuthBrowserAuthorizationRequest BrowserRequest;
	FUnrealAIOAuthRevocationRequest RevocationRequest;
	FUnrealAISecretValue RevocationToken;
	TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> Executor;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAICancellationSource Cancellation;
	FUnrealAIDeadline Deadline;
	double DeadlinePollSeconds = 0.01;
	TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> CompletionSink;
	TFunction<void(const FUnrealAIRequestId &)> OnTerminal;
	TFunction<void(const FUnrealAIRequestId &)> OnPhysical;
	FUnrealAITerminalGuard Terminal;
};

class FOAuthAuthorizationCoordinatorState final
	: public TSharedFromThis<FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe>
{
  public:
	FOAuthAuthorizationCoordinatorState(TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
										TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
										const FUnrealAIOAuthAuthorizationCoordinatorConfig &InConfig)
		: Executor(MoveTemp(InExecutor)), Clock(MoveTemp(InClock)), Config(InConfig)
	{
		FString Error;
		bShutdown = !Config.ValidateShape(Error);
	}

	bool
	StartAuthorizeBrowserPkce(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
							  TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> CompletionSink,
							  const FUnrealAICancellationToken &Cancellation,
							  TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							  FUnrealAIProviderAccessError &OutError)
	{
		OutHandle.Reset();
		OutError = {};
		FString ShapeError;
		if (!Request.ValidateShape(ShapeError) || !Cancellation.IsValid())
		{
			OutError = MakeOAuthCoordinatorError(EUnrealAIErrorCategory::InvalidArgument,
												 EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError = MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Cancelled,
												 EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}

		const TWeakPtr<FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe> WeakSelf = AsShared();
		const TSharedRef<FOAuthAuthorizationOperation, ESPMode::ThreadSafe> Operation =
			MakeShared<FOAuthAuthorizationOperation, ESPMode::ThreadSafe>(
				Request, Executor, Clock, MoveTemp(CompletionSink), Cancellation, Config.DeadlinePollSeconds,
				[WeakSelf](const FUnrealAIRequestId &RequestId)
				{
					if (const TSharedPtr<FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe> Self =
							WeakSelf.Pin())
					{
						Self->OnTerminal(RequestId);
					}
				},
				[WeakSelf](const FUnrealAIRequestId &RequestId)
				{
					if (const TSharedPtr<FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe> Self =
							WeakSelf.Pin())
					{
						Self->OnPhysical(RequestId);
					}
				});
		return AdmitAndStart(Operation, OutHandle, OutError);
	}

	bool StartRevoke(const FUnrealAIOAuthRevocationRequest &Request, FUnrealAISecretValue &&Token,
					 TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> CompletionSink,
					 const FUnrealAICancellationToken &Cancellation,
					 TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					 FUnrealAIProviderAccessError &OutError)
	{
		ON_SCOPE_EXIT
		{
			Token.Reset();
		};
		OutHandle.Reset();
		OutError = {};
		FString ShapeError;
		if (!Request.ValidateShape(ShapeError) || !Token.IsSet() || !Cancellation.IsValid())
		{
			OutError = MakeOAuthCoordinatorError(EUnrealAIErrorCategory::InvalidArgument,
												 EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Cancellation.IsCancellationRequested())
		{
			OutError = MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Cancelled,
												 EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}

		const TWeakPtr<FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe> WeakSelf = AsShared();
		const TSharedRef<FOAuthAuthorizationOperation, ESPMode::ThreadSafe> Operation =
			MakeShared<FOAuthAuthorizationOperation, ESPMode::ThreadSafe>(
				Request, MoveTemp(Token), Executor, Clock, MoveTemp(CompletionSink), Cancellation,
				Config.DeadlinePollSeconds,
				[WeakSelf](const FUnrealAIRequestId &RequestId)
				{
					if (const TSharedPtr<FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe> Self =
							WeakSelf.Pin())
					{
						Self->OnTerminal(RequestId);
					}
				},
				[WeakSelf](const FUnrealAIRequestId &RequestId)
				{
					if (const TSharedPtr<FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe> Self =
							WeakSelf.Pin())
					{
						Self->OnPhysical(RequestId);
					}
				});
		return AdmitAndStart(Operation, OutHandle, OutError);
	}

	void BeginShutdown()
	{
		TArray<TSharedPtr<FOAuthAuthorizationOperation, ESPMode::ThreadSafe>> Pending;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown)
			{
				return;
			}
			bShutdown = true;
			ActiveOperations.GenerateValueArray(Pending);
		}
		for (const TSharedPtr<FOAuthAuthorizationOperation, ESPMode::ThreadSafe> &Operation : Pending)
		{
			if (Operation.IsValid())
			{
				Operation->CancelForShutdown();
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

	int32 PumpDeadlines()
	{
		TArray<TSharedPtr<FOAuthAuthorizationOperation, ESPMode::ThreadSafe>> Pending;
		{
			FScopeLock Lock(&Mutex);
			ActiveOperations.GenerateValueArray(Pending);
		}
		int32 Settled = 0;
		for (const TSharedPtr<FOAuthAuthorizationOperation, ESPMode::ThreadSafe> &Operation : Pending)
		{
			if (Operation.IsValid() && Operation->TryObserveLogicalTerminal())
			{
				++Settled;
			}
		}
		return Settled;
	}

  private:
	bool AdmitAndStart(const TSharedRef<FOAuthAuthorizationOperation, ESPMode::ThreadSafe> &Operation,
					   TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					   FUnrealAIProviderAccessError &OutError)
	{
		const FGuid RequestGuid = Operation->GetRequestId().Value;
		{
			FScopeLock Lock(&Mutex);
			if (bShutdown || ActiveOperations.Contains(RequestGuid) || PhysicalOperationIds.Contains(RequestGuid) ||
				PhysicalOperationIds.Num() >= Config.MaxActiveOperations)
			{
				OutError = MakeOAuthCoordinatorError(EUnrealAIErrorCategory::Busy,
													 EUnrealAIProviderAccessErrorCode::OperationBusy, true);
				return false;
			}
			ActiveOperations.Add(RequestGuid, Operation);
			PhysicalOperationIds.Add(RequestGuid);
		}

		OutHandle = Operation;
		(void)Async(EAsyncExecution::Thread, [Operation]() { Operation->Run(); });
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
		if (!Config.bDisableBackgroundDeadlineMonitorForTesting)
#endif
		{
			(void)Async(EAsyncExecution::Thread, [Operation]() { Operation->Monitor(); });
		}
		return true;
	}

	void OnTerminal(const FUnrealAIRequestId &RequestId)
	{
		FScopeLock Lock(&Mutex);
		ActiveOperations.Remove(RequestId.Value);
	}

	void OnPhysical(const FUnrealAIRequestId &RequestId)
	{
		FScopeLock Lock(&Mutex);
		PhysicalOperationIds.Remove(RequestId.Value);
	}

	TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> Executor;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAIOAuthAuthorizationCoordinatorConfig Config;
	mutable FCriticalSection Mutex;
	TMap<FGuid, TSharedPtr<FOAuthAuthorizationOperation, ESPMode::ThreadSafe>> ActiveOperations;
	TSet<FGuid> PhysicalOperationIds;
	bool bShutdown = false;
};
} // namespace UE::UnrealAI::Private

bool FUnrealAIOAuthAuthorizationCompletion::IsSuccess() const
{
	return TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::Succeeded;
}

bool FUnrealAIOAuthAuthorizationCompletion::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString ErrorShape;
	const bool bKnownOperation = OperationKind == EUnrealAIOAuthAuthorizationOperationKind::AuthorizeBrowserPkce ||
								 OperationKind == EUnrealAIOAuthAuthorizationOperationKind::Revoke;
	const bool bSuccess = TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::Succeeded;
	const bool bFailure = TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::Failed ||
						  TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::Cancelled ||
						  TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::TimedOut;
	if (!RequestId.IsValid() || !bKnownOperation || (!bSuccess && !bFailure))
	{
		OutError = TEXT("OAuth authorization completion identity or terminal kind is invalid.");
		return false;
	}
	if (bSuccess)
	{
		const bool bAuthorizationResultValid =
			OperationKind == EUnrealAIOAuthAuthorizationOperationKind::AuthorizeBrowserPkce
				? AuthorizationResult.Tokens.AccessToken.IsSet() && AuthorizationResult.Tokens.IdToken.IsSet() &&
					  !AuthorizationResult.SubjectFingerprint.IsEmpty() && !AuthorizationResult.GrantedScopes.IsEmpty()
				: IsEmptyAuthorizationResult(AuthorizationResult);
		if (Error.IsError() || !bAuthorizationResultValid)
		{
			OutError = TEXT("Successful OAuth authorization completion has inconsistent result or error fields.");
			return false;
		}
		return true;
	}
	if (!IsEmptyAuthorizationResult(AuthorizationResult) || !Error.IsError() || !Error.ValidateShape(ErrorShape) ||
		(TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::Cancelled &&
		 (Error.Category != EUnrealAIErrorCategory::Cancelled ||
		  Error.Code != EUnrealAIProviderAccessErrorCode::AuthCancelled)) ||
		(TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::TimedOut &&
		 (Error.Category != EUnrealAIErrorCategory::Timeout ||
		  Error.Code != EUnrealAIProviderAccessErrorCode::AuthTimedOut)) ||
		(TerminalKind == EUnrealAIOAuthAuthorizationTerminalKind::Failed &&
		 (Error.Category == EUnrealAIErrorCategory::Cancelled || Error.Category == EUnrealAIErrorCategory::Timeout)))
	{
		OutError = TEXT("Failed OAuth authorization completion has inconsistent result or error fields.");
		return false;
	}
	return true;
}

bool FUnrealAIOAuthAuthorizationCoordinatorConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (MaxActiveOperations <= 0 || MaxActiveOperations > MaxActiveOperationsLimit ||
		!FMath::IsFinite(DeadlinePollSeconds) || DeadlinePollSeconds < MinDeadlinePollSeconds ||
		DeadlinePollSeconds > MaxDeadlinePollSeconds)
	{
		OutError = TEXT("OAuth authorization coordinator bounds are invalid.");
		return false;
	}
	return true;
}

FUnrealAIOAuthAuthorizationCoordinator::FUnrealAIOAuthAuthorizationCoordinator(
	TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
	const FUnrealAIOAuthAuthorizationCoordinatorConfig &InConfig)
	: State(MakeShared<UE::UnrealAI::Private::FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe>(
		  MoveTemp(InExecutor), MoveTemp(InClock), InConfig))
{
}

FUnrealAIOAuthAuthorizationCoordinator::~FUnrealAIOAuthAuthorizationCoordinator()
{
	BeginShutdown();
}

bool FUnrealAIOAuthAuthorizationCoordinator::StartAuthorizeBrowserPkce(
	const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
	TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> CompletionSink,
	const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> &OutHandle,
	FUnrealAIProviderAccessError &OutError)
{
	return State->StartAuthorizeBrowserPkce(Request, MoveTemp(CompletionSink), Cancellation, OutHandle, OutError);
}

bool FUnrealAIOAuthAuthorizationCoordinator::StartRevoke(
	const FUnrealAIOAuthRevocationRequest &Request, FUnrealAISecretValue &&Token,
	TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> CompletionSink,
	const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> &OutHandle,
	FUnrealAIProviderAccessError &OutError)
{
	return State->StartRevoke(Request, MoveTemp(Token), MoveTemp(CompletionSink), Cancellation, OutHandle, OutError);
}

void FUnrealAIOAuthAuthorizationCoordinator::BeginShutdown()
{
	State->BeginShutdown();
}

bool FUnrealAIOAuthAuthorizationCoordinator::IsShutdown() const
{
	return State->IsShutdown();
}

int32 FUnrealAIOAuthAuthorizationCoordinator::GetActiveOperationCount() const
{
	return State->GetActiveOperationCount();
}

int32 FUnrealAIOAuthAuthorizationCoordinator::GetPhysicalOperationCount() const
{
	return State->GetPhysicalOperationCount();
}

int32 FUnrealAIOAuthAuthorizationCoordinator::PumpDeadlines()
{
	return State->PumpDeadlines();
}
