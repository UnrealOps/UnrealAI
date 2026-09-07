// Copyright UnrealOps. All Rights Reserved.

// This guard precedes Unreal headers, so PLATFORM_MAC may be undefined.
#if defined(PLATFORM_MAC) && PLATFORM_MAC

// CoreServices declares its own FVector. Match Unreal's Mac system-header boundary before including Core types.
#define FVector FVectorWorkaround
#import <Foundation/Foundation.h>
#undef check
#undef verify
#undef FVector

#include "Transport/UnrealAIMacUrlSessionTransport.h"

#include "Misc/ScopeLock.h"

#include <dispatch/dispatch.h>

@class FUnrealAIMacUrlSessionDelegate;

namespace UE::UnrealAI::Transport::Private
{
class FMacRequestOperation;

struct FMacStagedInvalidation final
{
	bool bIsSet = false;
	bool bHasError = false;
	FString ErrorDomain;
	int64 ErrorCode = 0;
};

struct FMacAdmissionCommitPlan final
{
	dispatch_source_t SuspendedMonitor = nullptr;
	FMacStagedInvalidation StagedInvalidation;
	bool bReplayStagedInvalidation = false;
};

/** Synchronizes every native delegate read/write of the retained operation. */
class FMacDelegateCallbackState final
{
  public:
	TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> PinOperation() const
	{
		FScopeLock Lock(&Mutex);
		return Operation;
	}

	bool TryBindOperation(const TSharedRef<FMacRequestOperation, ESPMode::ThreadSafe> &InOperation,
						  FMacStagedInvalidation &OutInvalidation)
	{
		FScopeLock Lock(&Mutex);
		OutInvalidation = StagedInvalidation;
		if (StagedInvalidation.bIsSet || Operation.IsValid())
		{
			return false;
		}
		Operation = InOperation;
		return true;
	}

	TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> ConsumeOperationOrStageInvalidation(NSError *Error)
	{
		FMacStagedInvalidation Candidate;
		Candidate.bIsSet = true;
		Candidate.bHasError = Error != nil;
		if (Error != nil)
		{
			const char *Domain = [Error.domain UTF8String];
			if (Domain != nullptr)
			{
				Candidate.ErrorDomain = UTF8_TO_TCHAR(Domain);
			}
			Candidate.ErrorCode = static_cast<int64>(Error.code);
		}

		FScopeLock Lock(&Mutex);
		TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> Result = MoveTemp(Operation);
		if (!Result.IsValid() && !StagedInvalidation.bIsSet)
		{
			StagedInvalidation = MoveTemp(Candidate);
		}
		return Result;
	}

	void ClearOperation()
	{
		FScopeLock Lock(&Mutex);
		Operation.Reset();
	}

  private:
	mutable FCriticalSection Mutex;
	TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> Operation;
	FMacStagedInvalidation StagedInvalidation;
};

FUnrealAIModelError MakeTransportError(const EUnrealAIErrorCategory Category, const TCHAR *Code,
									   const TCHAR *UserMessage, const bool bRetryable = false)
{
	FUnrealAIModelError Error;
	Error.Category = Category;
	Error.Code = FName(Code);
	Error.UserMessage = FText::FromString(UserMessage);
	Error.bRetryable = bRetryable;
	return Error;
}

EUnrealAITerminalKind ToTerminalKind(const EUnrealAIHttpEventKind Kind)
{
	switch (Kind)
	{
	case EUnrealAIHttpEventKind::Completed:
		return EUnrealAITerminalKind::Succeeded;
	case EUnrealAIHttpEventKind::Failed:
		return EUnrealAITerminalKind::Failed;
	case EUnrealAIHttpEventKind::Cancelled:
		return EUnrealAITerminalKind::Cancelled;
	case EUnrealAIHttpEventKind::TimedOut:
		return EUnrealAITerminalKind::TimedOut;
	default:
		return EUnrealAITerminalKind::None;
	}
}

class FMacTransportState final : public TSharedFromThis<FMacTransportState, ESPMode::ThreadSafe>
{
  public:
	explicit FMacTransportState(FUnrealAIHttpTransportOptions InOptions) : Options(MoveTemp(InOptions))
	{
		check(Options.Clock.IsValid());
	}

	bool StartRequest(const FUnrealAIHttpRequest &Request,
					  const TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> &AccessContext,
					  const TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> &EventSink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle, FString &OutError);
	void PumpDeadlines();
	void BeginShutdown();
	void RemoveActiveRequest(const FUnrealAIRequestId &RequestId, const FMacRequestOperation *ExpectedOperation);

  private:
	FCriticalSection Mutex;
	FUnrealAIHttpTransportOptions Options;
	/** IDs reserved against duplicate/capacity checks before clock or native construction. */
	TSet<FGuid> ReservedRequestIds;
	TMap<FGuid, TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe>> ActiveRequests;
	/** Exact subset of ActiveRequests that crossed the true-admission commit point. */
	TSet<FGuid> CommittedRequestIds;
	bool bShuttingDown = false;
};

class FMacRequestOperation final : public IUnrealAICredentialApplicator,
								   public TSharedFromThis<FMacRequestOperation, ESPMode::ThreadSafe>
{
  public:
	FMacRequestOperation(FUnrealAIHttpRequest InRequest,
						 TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> InEventSink,
						 FUnrealAICancellationToken InCancellation,
						 TWeakPtr<FMacTransportState, ESPMode::ThreadSafe> InOwner,
						 TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
						 const double InCancellationPollSeconds, const bool bInSuspendNativeTaskForTesting);
	~FMacRequestOperation() override;

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Request.Destination;
	}

	FUnrealAIRequestId GetRequestId() const
	{
		return Request.RequestId;
	}

	bool IsPrepared() const;
	bool ReserveAdmissionCommit(FMacAdmissionCommitPlan &OutPlan);
	void CompleteAdmissionCommit(FMacAdmissionCommitPlan &&Plan);
	bool IsPhysicallySettled() const;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	int32 GetPhysicalCancellationCountForTesting() const;
	bool EmitResponseStartedForTesting();
	void CompleteSuccessfullyForTesting();
	bool EmitProvisionalInvalidationForTesting();
#endif
	void AbandonBeforeAdmission();
	void CancelByCaller();
	void CancelForShutdown();
	void PollCancellationAndDeadline();

	void OnRedirectRejected();
	void OnAuthenticationChallengeRejected();
	bool OnResponseStarted(const FUnrealAIHttpResponseMetadata &Metadata);
	enum class EBodyResult : uint8
	{
		Accepted,
		Ignored,
		TooLarge,
		InvalidSequence
	};
	EBodyResult OnBodyData(const uint8 *Bytes, int64 NumBytes);
	void OnTaskCompleted(NSError *Error);
	void OnSessionInvalidated(NSError *Error);

  protected:
	bool ApplyCredentialAndDispatch(EUnrealAIAuthScheme Scheme, TConstArrayView<uint8> Secret) override;
	bool ApplyCredentialAndProtectedSecondaryAndDispatch(EUnrealAIAuthScheme Scheme, TConstArrayView<uint8> Secret,
														 TConstArrayView<uint8> ProtectedSecondary) override;
	bool DispatchWithoutCredential() override;

  private:
	bool PrepareNativeRequest(EUnrealAIAuthScheme Scheme, TConstArrayView<uint8> Secret,
							  TConstArrayView<uint8> ProtectedSecondary);
	void Finish(EUnrealAIHttpEventKind Kind, FUnrealAIModelError Error);
	void PublishTerminalAfterPhysicalSettlement(NSError *InvalidationError);
	void QueueEventLocked(FUnrealAIHttpEvent &&Event);
	void InvalidateNativeObjectsBeforeAdmission();

	mutable FCriticalSection Mutex;
	FUnrealAIHttpRequest Request;
	TSharedPtr<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> EventSink;
	FUnrealAICancellationToken Cancellation;
	TWeakPtr<FMacTransportState, ESPMode::ThreadSafe> Owner;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FUnrealAIDeadline Deadline;
	FUnrealAITerminalGuard TerminalGuard;
	TOptional<FUnrealAIHttpEvent> PendingTerminal;
	uint64 NextSequence = 0;
	int64 ReceivedBodyBytes = 0;
	bool bResponseStarted = false;
	bool bPrepared = false;
	bool bAdmissionCommitReserved = false;
	bool bNativeSourcesActivated = false;
	bool bNativeInvalidationStarted = false;
	bool bPhysicallySettled = false;
	bool bAbandonedBeforeAdmission = false;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	int32 PhysicalCancellationCount = 0;
#endif
	double CancellationPollSeconds = 0.05;
	bool bSuspendNativeTaskForTesting = false;
	dispatch_queue_t EventQueue = nullptr;
	dispatch_source_t Monitor = nullptr;
	FUnrealAIMacUrlSessionDelegate *NativeDelegate = nil;
	NSURLSession *Session = nil;
	NSURLSessionDataTask *Task = nil;
};
} // namespace UE::UnrealAI::Transport::Private

@interface FUnrealAIMacUrlSessionDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
{
  @public
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacDelegateCallbackState, ESPMode::ThreadSafe> CallbackState;
}
@end

namespace UE::UnrealAI::Transport::Private
{
FString CopyBoundedHeaderValue(NSString *Value, const int32 MaxUtf8Bytes)
{
	if (Value == nil)
	{
		return FString();
	}
	const char *Utf8 = [Value UTF8String];
	if (Utf8 == nullptr)
	{
		return FString();
	}
	const int64 Length = static_cast<int64>(FCStringAnsi::Strlen(Utf8));
	const NSUInteger NativeLength = [Value lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
	if (Length < 0 || Length > MaxUtf8Bytes || NativeLength != static_cast<NSUInteger>(Length))
	{
		return FString();
	}
	for (int64 Index = 0; Index < Length; ++Index)
	{
		if (Utf8[Index] == '\r' || Utf8[Index] == '\n' || Utf8[Index] == '\0')
		{
			return FString();
		}
	}
	return FString(UTF8_TO_TCHAR(Utf8));
}

float ParseRetryAfterSeconds(NSString *Value)
{
	if (Value == nil || [Value length] == 0 || [Value length] > 16)
	{
		return 0.0f;
	}
	NSScanner *Scanner = [NSScanner scannerWithString:Value];
	double Seconds = 0.0;
	if (![Scanner scanDouble:&Seconds] || ![Scanner isAtEnd] || !FMath::IsFinite(Seconds) || Seconds < 0.0 ||
		Seconds > 86400.0)
	{
		return 0.0f;
	}
	return static_cast<float>(Seconds);
}

NSString *MakeNSString(const FString &Value)
{
	FTCHARToUTF8 Utf8(*Value);
	return [[NSString alloc] initWithBytes:Utf8.Get()
									length:static_cast<NSUInteger>(Utf8.Length())
								  encoding:NSUTF8StringEncoding];
}
} // namespace UE::UnrealAI::Transport::Private

@implementation FUnrealAIMacUrlSessionDelegate
- (instancetype)init
{
	self = [super init];
	if (self != nil)
	{
		CallbackState = MakeShared<UE::UnrealAI::Transport::Private::FMacDelegateCallbackState, ESPMode::ThreadSafe>();
	}
	return self;
}

- (void)URLSession:(NSURLSession *)Session
						  task:(NSURLSessionTask *)Task
	willPerformHTTPRedirection:(NSHTTPURLResponse *)Response
					newRequest:(NSURLRequest *)Request
			 completionHandler:(void (^)(NSURLRequest *_Nullable))CompletionHandler
{
	CompletionHandler(nil);
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacRequestOperation, ESPMode::ThreadSafe> LocalOperation =
		CallbackState->PinOperation();
	if (LocalOperation.IsValid())
	{
		LocalOperation->OnRedirectRejected();
	}
}

- (void)URLSession:(NSURLSession *)Session
	didReceiveChallenge:(NSURLAuthenticationChallenge *)Challenge
	  completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition Disposition,
								  NSURLCredential *_Nullable Credential))CompletionHandler
{
	if ([Challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust])
	{
		CompletionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
		return;
	}

	CompletionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacRequestOperation, ESPMode::ThreadSafe> LocalOperation =
		CallbackState->PinOperation();
	if (LocalOperation.IsValid())
	{
		LocalOperation->OnAuthenticationChallengeRejected();
	}
}

- (void)URLSession:(NSURLSession *)Session
				   task:(NSURLSessionTask *)Task
	didReceiveChallenge:(NSURLAuthenticationChallenge *)Challenge
	  completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition Disposition,
								  NSURLCredential *_Nullable Credential))CompletionHandler
{
	if ([Challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust])
	{
		CompletionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
		return;
	}

	CompletionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacRequestOperation, ESPMode::ThreadSafe> LocalOperation =
		CallbackState->PinOperation();
	if (LocalOperation.IsValid())
	{
		LocalOperation->OnAuthenticationChallengeRejected();
	}
}

- (void)URLSession:(NSURLSession *)Session
			  dataTask:(NSURLSessionDataTask *)DataTask
	didReceiveResponse:(NSURLResponse *)Response
	 completionHandler:(void (^)(NSURLSessionResponseDisposition Disposition))CompletionHandler
{
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacRequestOperation, ESPMode::ThreadSafe> LocalOperation =
		CallbackState->PinOperation();
	NSHTTPURLResponse *HttpResponse =
		[Response isKindOfClass:[NSHTTPURLResponse class]] ? static_cast<NSHTTPURLResponse *>(Response) : nil;
	if (!LocalOperation.IsValid() || HttpResponse == nil)
	{
		CompletionHandler(NSURLSessionResponseCancel);
		if (LocalOperation.IsValid())
		{
			LocalOperation->OnTaskCompleted([NSError errorWithDomain:NSURLErrorDomain
																code:NSURLErrorBadServerResponse
															userInfo:nil]);
		}
		return;
	}

	FUnrealAIHttpResponseMetadata Metadata;
	Metadata.StatusCode = static_cast<int32>(HttpResponse.statusCode);
	Metadata.ContentType = UE::UnrealAI::Transport::Private::CopyBoundedHeaderValue(
		[HttpResponse valueForHTTPHeaderField:@"Content-Type"], FUnrealAIHttpResponseMetadata::MaxContentTypeBytes);
	Metadata.ProviderRequestId = UE::UnrealAI::Transport::Private::CopyBoundedHeaderValue(
		[HttpResponse valueForHTTPHeaderField:@"X-Request-ID"],
		FUnrealAIHttpResponseMetadata::MaxProviderRequestIdBytes);
	Metadata.RetryAfterSeconds =
		UE::UnrealAI::Transport::Private::ParseRetryAfterSeconds([HttpResponse valueForHTTPHeaderField:@"Retry-After"]);
	CompletionHandler(LocalOperation->OnResponseStarted(Metadata) ? NSURLSessionResponseAllow
																  : NSURLSessionResponseCancel);
}

- (void)URLSession:(NSURLSession *)Session dataTask:(NSURLSessionDataTask *)DataTask didReceiveData:(NSData *)Data
{
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacRequestOperation, ESPMode::ThreadSafe> LocalOperation =
		CallbackState->PinOperation();
	if (!LocalOperation.IsValid())
	{
		return;
	}
	const NSUInteger NativeLength = Data.length;
	if (NativeLength > static_cast<NSUInteger>(MAX_int64))
	{
		LocalOperation->OnTaskCompleted([NSError errorWithDomain:NSURLErrorDomain
															code:NSURLErrorDataLengthExceedsMaximum
														userInfo:nil]);
		return;
	}
	const auto Result =
		LocalOperation->OnBodyData(static_cast<const uint8 *>(Data.bytes), static_cast<int64>(NativeLength));
	if (Result == UE::UnrealAI::Transport::Private::FMacRequestOperation::EBodyResult::TooLarge)
	{
		LocalOperation->OnTaskCompleted([NSError errorWithDomain:NSURLErrorDomain
															code:NSURLErrorDataLengthExceedsMaximum
														userInfo:nil]);
	}
	else if (Result == UE::UnrealAI::Transport::Private::FMacRequestOperation::EBodyResult::InvalidSequence)
	{
		LocalOperation->OnTaskCompleted([NSError errorWithDomain:NSURLErrorDomain
															code:NSURLErrorBadServerResponse
														userInfo:nil]);
	}
}

- (void)URLSession:(NSURLSession *)Session task:(NSURLSessionTask *)Task didCompleteWithError:(NSError *)Error
{
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacRequestOperation, ESPMode::ThreadSafe> LocalOperation =
		CallbackState->PinOperation();
	if (LocalOperation.IsValid())
	{
		LocalOperation->OnTaskCompleted(Error);
	}
}

- (void)URLSession:(NSURLSession *)Session
			 dataTask:(NSURLSessionDataTask *)DataTask
	willCacheResponse:(NSCachedURLResponse *)ProposedResponse
	completionHandler:(void (^)(NSCachedURLResponse *_Nullable CachedResponse))CompletionHandler
{
	CompletionHandler(nil);
}

- (void)URLSession:(NSURLSession *)Session didBecomeInvalidWithError:(NSError *)Error
{
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacRequestOperation, ESPMode::ThreadSafe> LocalOperation =
		CallbackState->ConsumeOperationOrStageInvalidation(Error);
	if (LocalOperation.IsValid())
	{
		LocalOperation->OnSessionInvalidated(Error);
	}
}
@end

namespace UE::UnrealAI::Transport::Private
{
class FMacRequestHandle final : public IUnrealAIHttpRequestHandle
{
  public:
	explicit FMacRequestHandle(const TSharedRef<FMacRequestOperation, ESPMode::ThreadSafe> &InOperation)
		: RequestId(InOperation->GetRequestId()), Operation(InOperation)
	{
	}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}

	bool IsPhysicallySettled() const override
	{
		return Operation.IsValid() && Operation->IsPhysicallySettled();
	}

	void Cancel() override
	{
		if (Operation.IsValid())
		{
			Operation->CancelByCaller();
		}
	}

#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	int32 GetPhysicalCancellationCountForTesting() const override
	{
		return Operation.IsValid() ? Operation->GetPhysicalCancellationCountForTesting() : -1;
	}

	bool EmitResponseStartedForTesting() override
	{
		return Operation.IsValid() && Operation->EmitResponseStartedForTesting();
	}

	bool CompleteSuccessfullyForTesting() override
	{
		if (!Operation.IsValid())
		{
			return false;
		}
		Operation->CompleteSuccessfullyForTesting();
		return true;
	}
#endif

  private:
	FUnrealAIRequestId RequestId;
	TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> Operation;
};

FMacRequestOperation::FMacRequestOperation(FUnrealAIHttpRequest InRequest,
										   TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> InEventSink,
										   FUnrealAICancellationToken InCancellation,
										   TWeakPtr<FMacTransportState, ESPMode::ThreadSafe> InOwner,
										   TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
										   const double InCancellationPollSeconds,
										   const bool bInSuspendNativeTaskForTesting)
	: Request(MoveTemp(InRequest)), EventSink(MoveTemp(InEventSink)), Cancellation(MoveTemp(InCancellation)),
	  Owner(MoveTemp(InOwner)), Clock(MoveTemp(InClock)),
	  Deadline(FUnrealAIDeadline::FromNow(*Clock, Request.TimeoutSeconds)),
	  CancellationPollSeconds(InCancellationPollSeconds), bSuspendNativeTaskForTesting(bInSuspendNativeTaskForTesting)
{
	EventQueue = dispatch_queue_create("com.unrealops.unrealai.transport.events", DISPATCH_QUEUE_SERIAL);
}

FMacRequestOperation::~FMacRequestOperation()
{
	if (Monitor != nullptr)
	{
		if (!bNativeSourcesActivated)
		{
			dispatch_resume(Monitor);
		}
		dispatch_source_cancel(Monitor);
		Monitor = nullptr;
	}
}

bool FMacRequestOperation::IsPrepared() const
{
	FScopeLock Lock(&Mutex);
	return bPrepared && !bAbandonedBeforeAdmission && !bPhysicallySettled && !TerminalGuard.IsComplete();
}

bool FMacRequestOperation::ReserveAdmissionCommit(FMacAdmissionCommitPlan &OutPlan)
{
	OutPlan = FMacAdmissionCommitPlan();
	FScopeLock Lock(&Mutex);
	if (!bPrepared || bAbandonedBeforeAdmission || bAdmissionCommitReserved || bNativeSourcesActivated ||
		Monitor == nullptr || NativeDelegate == nil || Session == nil || Task == nil)
	{
		return false;
	}

	// This method runs while the transport state still owns the admission
	// linearization lock. It may only reserve internal ownership; it must not
	// invoke clocks, native work, sinks, or provider callbacks.
	FMacStagedInvalidation StagedInvalidation;
	const bool bDelegateBound = NativeDelegate->CallbackState->TryBindOperation(AsShared(), StagedInvalidation);
	bAdmissionCommitReserved = true;
	if (bDelegateBound)
	{
		return true;
	}

	check(StagedInvalidation.bIsSet);
	const bool bTerminalReserved = TerminalGuard.TryComplete(EUnrealAITerminalKind::Failed);
	check(bTerminalReserved);
	if (!bTerminalReserved)
	{
		return false;
	}
	FUnrealAIHttpEvent Event;
	Event.RequestId = Request.RequestId;
	Event.Kind = EUnrealAIHttpEventKind::Failed;
	Event.Error = MakeTransportError(EUnrealAIErrorCategory::Transport,
									 TEXT("TransportInvalidated"),
										  TEXT("The model transport closed before the request completed."), true);
	PendingTerminal = MoveTemp(Event);

	// The native callback already proved physical invalidation. Reserve both
	// logical and physical winners before the state exposes this operation as
	// committed, making deadline, cancellation, and shutdown Finish calls inert.
	bNativeInvalidationStarted = true;
	bNativeSourcesActivated = true;
	OutPlan.SuspendedMonitor = Monitor;
	Monitor = nullptr;
	OutPlan.StagedInvalidation = MoveTemp(StagedInvalidation);
	OutPlan.bReplayStagedInvalidation = true;
	return true;
}

void FMacRequestOperation::CompleteAdmissionCommit(FMacAdmissionCommitPlan &&Plan)
{
	if (Plan.bReplayStagedInvalidation)
	{
		check(Plan.SuspendedMonitor != nullptr);
		// The timer is still suspended here. Remove its operation-entering
		// handler before balancing the dispatch source lifetime, so neither an
		// expired deadline nor cancellation can contend with the reserved
		// invalidation terminal.
		dispatch_source_set_event_handler(Plan.SuspendedMonitor, ^{
										  });
		dispatch_resume(Plan.SuspendedMonitor);
		dispatch_source_cancel(Plan.SuspendedMonitor);
		NSError *ReplayError = nil;
		if (Plan.StagedInvalidation.bHasError)
		{
			NSString *Domain = Plan.StagedInvalidation.ErrorDomain.IsEmpty()
								   ? NSURLErrorDomain
								   : MakeNSString(Plan.StagedInvalidation.ErrorDomain);
			ReplayError = [NSError errorWithDomain:Domain
											  code:static_cast<NSInteger>(Plan.StagedInvalidation.ErrorCode)
										  userInfo:nil];
		}
		OnSessionInvalidated(ReplayError);
		return;
	}

	dispatch_source_t LocalMonitor = nullptr;
	NSURLSessionDataTask *LocalTask = nil;
	NSURLSession *TerminalSession = nil;
	bool bFinishInvalidation = false;
	{
		FScopeLock Lock(&Mutex);
		if (!bAdmissionCommitReserved || !bPrepared || bAbandonedBeforeAdmission || bPhysicallySettled ||
			bNativeSourcesActivated || Monitor == nullptr || NativeDelegate == nil || Session == nil || Task == nil)
		{
			return;
		}

		bNativeSourcesActivated = true;
		LocalMonitor = Monitor;
		// Shutdown or another committed terminal can win between the state
		// commit and native activation. Its physical invalidation was deferred
		// until the exact delegate binding was reserved.
		if (TerminalGuard.IsComplete())
		{
			Monitor = nullptr;
			if (!bNativeInvalidationStarted)
			{
				bNativeInvalidationStarted = true;
				TerminalSession = Session;
				bFinishInvalidation =
					PendingTerminal.IsSet() && PendingTerminal->Kind == EUnrealAIHttpEventKind::Completed;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
				if (TerminalSession != nil && !bFinishInvalidation)
				{
					++PhysicalCancellationCount;
				}
#endif
			}
		}
		else
		{
			LocalTask = Task;
		}
		dispatch_resume(LocalMonitor);
	}

	if (TerminalSession != nil)
	{
		dispatch_source_set_event_handler(LocalMonitor, ^{
										  });
		dispatch_source_cancel(LocalMonitor);
		if (bFinishInvalidation)
		{
			[TerminalSession finishTasksAndInvalidate];
		}
		else
		{
			[TerminalSession invalidateAndCancel];
		}
		return;
	}

	// The delegate is now bound and the cancellation monitor is active, so an
	// exact fake-clock deadline or cancellation can safely initiate native
	// invalidation and receive its conclusive callback.
	PollCancellationAndDeadline();
	{
		FScopeLock Lock(&Mutex);
		if (!TerminalGuard.IsComplete() && !bSuspendNativeTaskForTesting && Task == LocalTask)
		{
			[LocalTask resume];
		}
	}
}

#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
bool FMacRequestOperation::EmitProvisionalInvalidationForTesting()
{
	FUnrealAIMacUrlSessionDelegate *LocalDelegate = nil;
	NSURLSession *LocalSession = nil;
	{
		FScopeLock Lock(&Mutex);
		if (!bPrepared || bAbandonedBeforeAdmission || bNativeSourcesActivated || NativeDelegate == nil ||
			Session == nil)
		{
			return false;
		}
		LocalDelegate = NativeDelegate;
		LocalSession = Session;
	}

	// Invoke the exact native invalidation callback seam while the request is
	// provisional. The delegate target must still be nil, so this cannot enter
	// the operation or authorize provider-facing delivery.
	NSError *InvalidationError = [NSError errorWithDomain:NSURLErrorDomain
													 code:NSURLErrorNetworkConnectionLost
												 userInfo:nil];
	[LocalSession invalidateAndCancel];
	[LocalDelegate URLSession:LocalSession didBecomeInvalidWithError:InvalidationError];
	return true;
}
#endif

bool FMacRequestOperation::IsPhysicallySettled() const
{
	FScopeLock Lock(&Mutex);
	return bPhysicallySettled;
}

#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
int32 FMacRequestOperation::GetPhysicalCancellationCountForTesting() const
{
	FScopeLock Lock(&Mutex);
	return PhysicalCancellationCount;
}

bool FMacRequestOperation::EmitResponseStartedForTesting()
{
	FUnrealAIHttpResponseMetadata Metadata;
	Metadata.StatusCode = 200;
	Metadata.ContentType = TEXT("application/json");
	Metadata.ProviderRequestId = TEXT("test_native_response");
	return OnResponseStarted(Metadata);
}

void FMacRequestOperation::CompleteSuccessfullyForTesting()
{
	OnTaskCompleted(nil);
}
#endif

bool FMacRequestOperation::ApplyCredentialAndDispatch(const EUnrealAIAuthScheme Scheme,
													  const TConstArrayView<uint8> Secret)
{
	return PrepareNativeRequest(Scheme, Secret, {});
}

bool FMacRequestOperation::ApplyCredentialAndProtectedSecondaryAndDispatch(
	const EUnrealAIAuthScheme Scheme, const TConstArrayView<uint8> Secret,
	const TConstArrayView<uint8> ProtectedSecondary)
{
	return PrepareNativeRequest(Scheme, Secret, ProtectedSecondary);
}

bool FMacRequestOperation::DispatchWithoutCredential()
{
	return false;
}

bool FMacRequestOperation::PrepareNativeRequest(const EUnrealAIAuthScheme Scheme, const TConstArrayView<uint8> Secret,
												const TConstArrayView<uint8> ProtectedSecondary)
{
	const bool bSingleBearer =
		Request.CredentialPresentation == EUnrealAIHttpCredentialPresentation::AuthorizationBearer &&
		Request.ProtectedSecondaryHeaderName.IsEmpty() && ProtectedSecondary.IsEmpty() &&
		(Scheme == EUnrealAIAuthScheme::ApiKey || Scheme == EUnrealAIAuthScheme::OAuthBearer ||
		 Scheme == EUnrealAIAuthScheme::GatewayBearer);
	const bool bAnthropicApiKey =
		Request.CredentialPresentation == EUnrealAIHttpCredentialPresentation::AnthropicApiKeyHeader &&
		Request.ProtectedSecondaryHeaderName.IsEmpty() && ProtectedSecondary.IsEmpty() &&
		Request.Destination.ModelProviderName ==
			TEXT("anthropic.messages") && Scheme == EUnrealAIAuthScheme::ApiKey &&
				 Request.Destination.BillingMode == EUnrealAIBillingMode::ApiMetered &&
				 Request.Destination.EndpointOrigin.ToString() == TEXT("https://api.anthropic.com");
	const bool bGeminiApiKey =
		Request.CredentialPresentation == EUnrealAIHttpCredentialPresentation::GeminiApiKeyHeader &&
		Request.ProtectedSecondaryHeaderName.IsEmpty() && ProtectedSecondary.IsEmpty() &&
		Request.Destination.ModelProviderName ==
			TEXT("gemini.interactions") && Scheme == EUnrealAIAuthScheme::ApiKey &&
				 Request.Destination.BillingMode == EUnrealAIBillingMode::ApiMetered &&
				 Request.Destination.EndpointOrigin.ToString() == TEXT("https://generativelanguage.googleapis.com");
	const bool bBearerWithProtectedSecondary =
#if UE_BUILD_SHIPPING || UE_SERVER
		false;
#else
		Request.CredentialPresentation ==
			EUnrealAIHttpCredentialPresentation::AuthorizationBearerWithProtectedSecondary &&
		Scheme == EUnrealAIAuthScheme::OAuthBearer &&
		Request.Destination.BillingMode == EUnrealAIBillingMode::SubscriptionQuota &&
		!Request.ProtectedSecondaryHeaderName.IsEmpty() && !ProtectedSecondary.IsEmpty();
#endif
	if (Scheme != Request.Destination.AuthScheme ||
		(!bSingleBearer && !bAnthropicApiKey && !bGeminiApiKey && !bBearerWithProtectedSecondary) || Secret.IsEmpty() ||
		Secret.Num() > FUnrealAISecretValue::MaxSecretBytes ||
		ProtectedSecondary.Num() > FUnrealAISecretValue::MaxSecretBytes)
	{
		return false;
	}
	for (const uint8 Byte : Secret)
	{
		if (Byte < 0x21 || Byte > 0x7e)
		{
			return false;
		}
	}
	for (const uint8 Byte : ProtectedSecondary)
	{
		if (Byte < 0x21 || Byte > 0x7e)
		{
			return false;
		}
	}
	{
		FScopeLock Lock(&Mutex);
		if (bAbandonedBeforeAdmission)
		{
			return false;
		}
	}
	if (Cancellation.IsCancellationRequested() || Deadline.IsExpired(*Clock))
	{
		return false;
	}

	@autoreleasepool
	{
		NSString *Origin = MakeNSString(Request.Destination.EndpointOrigin.ToString());
		NSString *Path = MakeNSString(Request.RelativePath);
		if (Origin == nil || Path == nil)
		{
			return false;
		}
		NSURLComponents *Components = [NSURLComponents componentsWithString:Origin];
		if (Components == nil || ![Components.scheme isEqualToString:@"https"] || Components.host.length == 0 ||
			Components.user != nil || Components.password != nil || Components.query != nil ||
			Components.fragment != nil)
		{
			return false;
		}
		Components.percentEncodedPath = Path;
		if (Request.QueryProfile == EUnrealAIHttpQueryProfile::GeminiAltSse)
		{
			if (!bGeminiApiKey || !Request.RelativePath.Equals(TEXT("/v1/interactions"), ESearchCase::CaseSensitive))
			{
				return false;
			}
			Components.queryItems = @[ [NSURLQueryItem queryItemWithName:@"alt" value:@"sse"] ];
		}
		else if (Request.QueryProfile != EUnrealAIHttpQueryProfile::None)
		{
			return false;
		}
		NSURL *Url = Components.URL;
		if (Url == nil || ![Url.scheme isEqualToString:@"https"])
		{
			return false;
		}

		NSMutableURLRequest *NativeRequest = [[NSMutableURLRequest alloc] initWithURL:Url];
		if (NativeRequest == nil)
		{
			return false;
		}
		NativeRequest.HTTPMethod = Request.Method == EUnrealAIHttpMethod::Post ? @"POST" : @"GET";
		NativeRequest.timeoutInterval = Request.TimeoutSeconds;
		NativeRequest.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
		NativeRequest.HTTPShouldHandleCookies = NO;
		for (const FUnrealAIHttpRequestHeader &Header : Request.Headers)
		{
			NSString *Name = MakeNSString(Header.Name);
			NSString *Value = MakeNSString(Header.Value);
			if (Name == nil || Value == nil)
			{
				return false;
			}
			[NativeRequest setValue:Value forHTTPHeaderField:Name];
		}

		NSString *SecretText = [[NSString alloc] initWithBytes:Secret.GetData()
														length:static_cast<NSUInteger>(Secret.Num())
													  encoding:NSASCIIStringEncoding];
		if (SecretText == nil)
		{
			return false;
		}
		if (bSingleBearer || bBearerWithProtectedSecondary)
		{
			NSString *AuthorizationValue = [[NSString alloc] initWithFormat:@"Bearer %@", SecretText];
			if (AuthorizationValue == nil)
			{
				return false;
			}
			[NativeRequest setValue:AuthorizationValue forHTTPHeaderField:@"Authorization"];
		}
		else if (bAnthropicApiKey)
		{
			[NativeRequest setValue:SecretText forHTTPHeaderField:@"x-api-key"];
		}
		else if (bGeminiApiKey)
		{
			[NativeRequest setValue:SecretText forHTTPHeaderField:@"x-goog-api-key"];
		}
		if (bBearerWithProtectedSecondary)
		{
#if !UE_BUILD_SHIPPING && !UE_SERVER
			NSString *ProtectedSecondaryHeaderName = MakeNSString(Request.ProtectedSecondaryHeaderName);
			NSString *ProtectedSecondaryValue =
				[[NSString alloc] initWithBytes:ProtectedSecondary.GetData()
										 length:static_cast<NSUInteger>(ProtectedSecondary.Num())
									   encoding:NSASCIIStringEncoding];
			if (ProtectedSecondaryHeaderName == nil || ProtectedSecondaryValue == nil)
			{
				return false;
			}
			[NativeRequest setValue:ProtectedSecondaryValue forHTTPHeaderField:ProtectedSecondaryHeaderName];
#endif
		}

		if (!Request.Body.IsEmpty())
		{
			NSData *BodyData = [NSData dataWithBytes:Request.Body.GetData()
											  length:static_cast<NSUInteger>(Request.Body.Num())];
			if (BodyData == nil)
			{
				return false;
			}
			NativeRequest.HTTPBody = BodyData;
		}

		NSURLSessionConfiguration *Configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		if (Configuration == nil)
		{
			return false;
		}
		Configuration.URLCache = nil;
		Configuration.HTTPCookieStorage = nil;
		Configuration.URLCredentialStorage = nil;
		Configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
		Configuration.HTTPShouldSetCookies = NO;
		Configuration.timeoutIntervalForRequest = Request.TimeoutSeconds;
		Configuration.timeoutIntervalForResource = Request.TimeoutSeconds;
		Configuration.TLSMinimumSupportedProtocolVersion = tls_protocol_version_TLSv12;
		Configuration.waitsForConnectivity = NO;

		FUnrealAIMacUrlSessionDelegate *Delegate = [[FUnrealAIMacUrlSessionDelegate alloc] init];
		NSOperationQueue *DelegateQueue = [[NSOperationQueue alloc] init];
		if (Delegate == nil || DelegateQueue == nil)
		{
			return false;
		}
		DelegateQueue.maxConcurrentOperationCount = 1;
		DelegateQueue.name = @"com.unrealops.unrealai.transport.delegate";
		NSURLSession *NewSession = [NSURLSession sessionWithConfiguration:Configuration
																 delegate:Delegate
															delegateQueue:DelegateQueue];
		NSURLSessionDataTask *NewTask = [NewSession dataTaskWithRequest:NativeRequest];
		if (NewSession == nil || NewTask == nil)
		{
			[NewSession invalidateAndCancel];
			return false;
		}
		TWeakPtr<FMacRequestOperation, ESPMode::ThreadSafe> WeakSelf = AsShared();
		dispatch_source_t NewMonitor =
			dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
		if (NewMonitor == nullptr)
		{
			[NewSession invalidateAndCancel];
			return false;
		}
		const uint64 IntervalNanoseconds =
			static_cast<uint64>(CancellationPollSeconds * static_cast<double>(NSEC_PER_SEC));
		dispatch_source_set_timer(NewMonitor, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64>(IntervalNanoseconds)),
								  IntervalNanoseconds, IntervalNanoseconds / 10);
		dispatch_source_set_event_handler(NewMonitor, ^{
		  if (const TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin())
		  {
			  Pinned->PollCancellationAndDeadline();
		  }
		});

		const bool bDeadlineExpired = Deadline.IsExpired(*Clock);
		FScopeLock Lock(&Mutex);
		if (bPrepared || bNativeSourcesActivated || TerminalGuard.IsComplete() ||
			Cancellation.IsCancellationRequested() || bDeadlineExpired || bAbandonedBeforeAdmission)
		{
			// This source was never resumed, so remove its provider callback
			// before balancing the dispatch lifetime on the rejection path.
			dispatch_source_set_event_handler(NewMonitor, ^{
											  });
			dispatch_resume(NewMonitor);
			dispatch_source_cancel(NewMonitor);
			[NewSession invalidateAndCancel];
			return false;
		}
		// Stage the native objects after every preparation step succeeds, but
		// keep the delegate target nil and both callback sources suspended until
		// the state-level admission commit.
		check(!Delegate->CallbackState->PinOperation().IsValid());
		NativeDelegate = Delegate;
		Session = NewSession;
		Task = NewTask;
		Monitor = NewMonitor;
		bPrepared = true;
		check(!bNativeSourcesActivated);
		Request.Body.Reset();
		return true;
	}
}

void FMacRequestOperation::PollCancellationAndDeadline()
{
	if (Cancellation.IsCancellationRequested())
	{
		if (Cancellation.GetReason() == EUnrealAICancellationReason::Timeout)
		{
			Finish(EUnrealAIHttpEventKind::TimedOut,
				   MakeTransportError(EUnrealAIErrorCategory::Timeout, TEXT("RequestTimedOut"),
																			TEXT("The model request timed out.")));
		}
		else
		{
			Finish(EUnrealAIHttpEventKind::Cancelled,
				   MakeTransportError(EUnrealAIErrorCategory::Cancelled,
									  TEXT("RequestCancelled"), TEXT("The model request was cancelled.")));
		}
		return;
	}
	if (Deadline.IsExpired(*Clock))
	{
		Finish(EUnrealAIHttpEventKind::TimedOut,
			   MakeTransportError(EUnrealAIErrorCategory::Timeout, TEXT("RequestTimedOut"),
																		TEXT("The model request timed out.")));
	}
}

void FMacRequestOperation::AbandonBeforeAdmission()
{
	InvalidateNativeObjectsBeforeAdmission();
}

void FMacRequestOperation::CancelByCaller()
{
	Finish(EUnrealAIHttpEventKind::Cancelled,
		   MakeTransportError(EUnrealAIErrorCategory::Cancelled, TEXT("RequestCancelled"),
																	  TEXT("The model request was cancelled.")));
}

void FMacRequestOperation::CancelForShutdown()
{
	Finish(EUnrealAIHttpEventKind::Cancelled,
		   MakeTransportError(EUnrealAIErrorCategory::Cancelled, TEXT("TransportShutdown"),
																	  TEXT("The model transport is shutting down.")));
}

void FMacRequestOperation::OnRedirectRejected()
{
	PollCancellationAndDeadline();
	if (TerminalGuard.IsComplete())
	{
		return;
	}
	Finish(EUnrealAIHttpEventKind::Failed,
		   MakeTransportError(EUnrealAIErrorCategory::Transport,
							  TEXT("RedirectRejected"), TEXT("The model endpoint attempted an HTTP redirect.")));
}

void FMacRequestOperation::OnAuthenticationChallengeRejected()
{
	PollCancellationAndDeadline();
	if (TerminalGuard.IsComplete())
	{
		return;
	}
	Finish(EUnrealAIHttpEventKind::Failed,
		   MakeTransportError(EUnrealAIErrorCategory::Transport,
							  TEXT("AuthenticationChallengeRejected"),
								   TEXT("The model endpoint requested an unsupported authentication challenge.")));
}

bool FMacRequestOperation::OnResponseStarted(const FUnrealAIHttpResponseMetadata &Metadata)
{
	PollCancellationAndDeadline();
	if (TerminalGuard.IsComplete())
	{
		return false;
	}

	FString ShapeError;
	if (!Metadata.ValidateShape(ShapeError))
	{
		Finish(EUnrealAIHttpEventKind::Failed,
			   MakeTransportError(EUnrealAIErrorCategory::ProviderProtocol,
								  TEXT("InvalidHttpResponse"),
									   TEXT("The model endpoint returned an invalid HTTP response.")));
		return false;
	}

	{
		FScopeLock Lock(&Mutex);
		if (TerminalGuard.IsComplete())
		{
			return false;
		}
		if (bResponseStarted)
		{
			return false;
		}
		bResponseStarted = true;
		FUnrealAIHttpEvent Event;
		Event.RequestId = Request.RequestId;
		Event.Kind = EUnrealAIHttpEventKind::ResponseStarted;
		Event.Response = Metadata;
		QueueEventLocked(MoveTemp(Event));
	}
	return true;
}

FMacRequestOperation::EBodyResult FMacRequestOperation::OnBodyData(const uint8 *Bytes, const int64 NumBytes)
{
	PollCancellationAndDeadline();
	if (TerminalGuard.IsComplete())
	{
		return EBodyResult::Ignored;
	}
	if (Bytes == nullptr || NumBytes <= 0)
	{
		return EBodyResult::Accepted;
	}

	FScopeLock Lock(&Mutex);
	if (TerminalGuard.IsComplete())
	{
		return EBodyResult::Ignored;
	}
	if (!bResponseStarted)
	{
		return EBodyResult::InvalidSequence;
	}
	if (NumBytes > Request.MaxResponseBodyBytes - ReceivedBodyBytes)
	{
		return EBodyResult::TooLarge;
	}
	ReceivedBodyBytes += NumBytes;

	int64 Offset = 0;
	while (Offset < NumBytes)
	{
		const int32 ChunkSize =
			static_cast<int32>(FMath::Min<int64>(static_cast<int64>(Request.MaxBodyChunkBytes), NumBytes - Offset));
		FUnrealAIHttpEvent Event;
		Event.RequestId = Request.RequestId;
		Event.Kind = EUnrealAIHttpEventKind::BodyChunk;
		Event.BodyChunk.Append(Bytes + Offset, ChunkSize);
		QueueEventLocked(MoveTemp(Event));
		Offset += ChunkSize;
	}
	return EBodyResult::Accepted;
}

void FMacRequestOperation::OnTaskCompleted(NSError *Error)
{
	// The native URLSession completion can race the explicit monitor. Recheck
	// the exact injected monotonic deadline at this delivery boundary so a
	// nil-error completion at or after the deadline cannot win as Completed.
	PollCancellationAndDeadline();
	if (TerminalGuard.IsComplete())
	{
		return;
	}

	if (Error == nil)
	{
		bool bHasResponse = false;
		{
			FScopeLock Lock(&Mutex);
			bHasResponse = bResponseStarted;
		}
		if (!bHasResponse)
		{
			Finish(EUnrealAIHttpEventKind::Failed,
				   MakeTransportError(EUnrealAIErrorCategory::ProviderProtocol,
									  TEXT("MissingHttpResponse"),
										   TEXT("The model endpoint completed without an HTTP response.")));
			return;
		}
		Finish(EUnrealAIHttpEventKind::Completed, FUnrealAIModelError());
		return;
	}

	if ([Error.domain isEqualToString:NSURLErrorDomain] && Error.code == NSURLErrorTimedOut)
	{
		Finish(EUnrealAIHttpEventKind::TimedOut,
			   MakeTransportError(EUnrealAIErrorCategory::Timeout, TEXT("RequestTimedOut"),
																		TEXT("The model request timed out.")));
		return;
	}
	if ([Error.domain isEqualToString:NSURLErrorDomain] && Error.code == NSURLErrorDataLengthExceedsMaximum)
	{
		Finish(EUnrealAIHttpEventKind::Failed,
			   MakeTransportError(EUnrealAIErrorCategory::Transport,
								  TEXT("ResponseTooLarge"),
									   TEXT("The model endpoint response exceeded its configured byte limit.")));
		return;
	}
	if ([Error.domain isEqualToString:NSURLErrorDomain] && Error.code == NSURLErrorBadServerResponse)
	{
		Finish(EUnrealAIHttpEventKind::Failed,
			   MakeTransportError(EUnrealAIErrorCategory::ProviderProtocol,
								  TEXT("InvalidHttpResponse"),
									   TEXT("The model endpoint returned an invalid HTTP response.")));
		return;
	}

	Finish(EUnrealAIHttpEventKind::Failed,
		   MakeTransportError(EUnrealAIErrorCategory::Transport,
							  TEXT("TransportFailed"), TEXT("The model endpoint request failed."), true));
}

void FMacRequestOperation::QueueEventLocked(FUnrealAIHttpEvent &&Event)
{
	if (!EventSink.IsValid())
	{
		return;
	}
	Event.Sequence = ++NextSequence;
	TSharedRef<FUnrealAIHttpEvent, ESPMode::ThreadSafe> QueuedEvent =
		MakeShared<FUnrealAIHttpEvent, ESPMode::ThreadSafe>(MoveTemp(Event));
	TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> Sink = EventSink.ToSharedRef();
	dispatch_async(EventQueue, ^{
	  Sink->EnqueueHttpEvent(MoveTemp(*QueuedEvent));
	});
}

void FMacRequestOperation::Finish(const EUnrealAIHttpEventKind Kind, FUnrealAIModelError Error)
{
	const EUnrealAITerminalKind TerminalKind = ToTerminalKind(Kind);
	if (TerminalKind == EUnrealAITerminalKind::None)
	{
		return;
	}

	dispatch_source_t LocalMonitor = nullptr;
	NSURLSession *LocalSession = nil;
	bool bPublishWithoutNativeSession = false;
	bool bResumeMonitorBeforeCancel = false;
	bool bDeferUntilAdmissionCommit = false;
	{
		FScopeLock Lock(&Mutex);
		if (!TerminalGuard.TryComplete(TerminalKind))
		{
			return;
		}
		FUnrealAIHttpEvent Event;
		Event.RequestId = Request.RequestId;
		Event.Kind = Kind;
		Event.Error = MoveTemp(Error);
		PendingTerminal = MoveTemp(Event);
		if (bPrepared && !bAbandonedBeforeAdmission && !bNativeSourcesActivated && Session != nil)
		{
			// The state can commit immediately before shutdown observes this
			// operation, while StartRequest has not yet activated native
			// sources. Preserve the terminal winner, but defer physical
			// invalidation to CompleteAdmissionCommit so its conclusive callback
			// cannot be lost.
			bDeferUntilAdmissionCommit = true;
		}
		if (bDeferUntilAdmissionCommit)
		{
			return;
		}
		LocalMonitor = Monitor;
		Monitor = nullptr;
		bResumeMonitorBeforeCancel = LocalMonitor != nullptr && !bNativeSourcesActivated;
		if (!bNativeInvalidationStarted)
		{
			bNativeInvalidationStarted = true;
			LocalSession = Session;
			bPublishWithoutNativeSession = LocalSession == nil;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
			if (LocalSession != nil && Kind != EUnrealAIHttpEventKind::Completed)
			{
				++PhysicalCancellationCount;
			}
#endif
		}
	}

	if (LocalMonitor != nullptr)
	{
		dispatch_source_set_event_handler(LocalMonitor, ^{
										  });
		if (bResumeMonitorBeforeCancel)
		{
			dispatch_resume(LocalMonitor);
		}
		dispatch_source_cancel(LocalMonitor);
	}
	if (LocalSession != nil)
	{
		if (Kind == EUnrealAIHttpEventKind::Completed)
		{
			[LocalSession finishTasksAndInvalidate];
		}
		else
		{
			// invalidateAndCancel is the single physical cancellation edge.
			// The provider-facing terminal is deferred until the delegate's
			// didBecomeInvalidWithError callback proves this source is closed.
			// NSURLSession guarantees that invalidation callback; intentionally
			// do not use a timer fallback that could release provider ownership
			// while a native delegate can still enter.
			[LocalSession invalidateAndCancel];
		}
	}
	else if (bPublishWithoutNativeSession)
	{
		// Prepared admissions always own a session. This fallback covers a
		// fail-closed nil-session state without waiting forever; because there
		// is no native source, publication is already physically settled.
		PublishTerminalAfterPhysicalSettlement(nil);
	}
}

void FMacRequestOperation::OnSessionInvalidated(NSError *Error)
{
	PublishTerminalAfterPhysicalSettlement(Error);
}

void FMacRequestOperation::PublishTerminalAfterPhysicalSettlement(NSError *InvalidationError)
{
	(void)InvalidationError;
	TOptional<FUnrealAIHttpEvent> TerminalToPublish;
	dispatch_source_t LocalMonitor = nullptr;
	bool bResumeMonitorBeforeCancel = false;
	bool bRemoveFromOwner = false;
	{
		FScopeLock Lock(&Mutex);
		if (bPhysicallySettled)
		{
			return;
		}
		bPhysicallySettled = true;
		bNativeInvalidationStarted = true;
		Session = nil;
		Task = nil;
		NativeDelegate = nil;
		LocalMonitor = Monitor;
		Monitor = nullptr;
		bResumeMonitorBeforeCancel = LocalMonitor != nullptr && !bNativeSourcesActivated;

		if (bAbandonedBeforeAdmission)
		{
			PendingTerminal.Reset();
			EventSink.Reset();
		}
		else
		{
			if (!TerminalGuard.IsComplete())
			{
				TerminalGuard.TryComplete(EUnrealAITerminalKind::Failed);
				FUnrealAIHttpEvent UnexpectedInvalidation;
				UnexpectedInvalidation.RequestId = Request.RequestId;
				UnexpectedInvalidation.Kind = EUnrealAIHttpEventKind::Failed;
				UnexpectedInvalidation.Error =
					MakeTransportError(EUnrealAIErrorCategory::Transport,
									   TEXT("TransportInvalidated"),
											TEXT("The model transport closed before the request completed."), true);
				PendingTerminal = MoveTemp(UnexpectedInvalidation);
			}

			if (PendingTerminal.IsSet())
			{
				TerminalToPublish = MoveTemp(PendingTerminal.GetValue());
				PendingTerminal.Reset();
			}
			if (TerminalToPublish.IsSet())
			{
				QueueEventLocked(MoveTemp(TerminalToPublish.GetValue()));
			}
			// QueueEventLocked captures the sink for the final ordered delivery.
			// The retained request handle can now keep the settled operation alive
			// without extending provider callback ownership.
			EventSink.Reset();
			bRemoveFromOwner = true;
		}
	}

	if (LocalMonitor != nullptr)
	{
		dispatch_source_set_event_handler(LocalMonitor, ^{
										  });
		if (bResumeMonitorBeforeCancel)
		{
			dispatch_resume(LocalMonitor);
		}
		dispatch_source_cancel(LocalMonitor);
	}
	if (bRemoveFromOwner)
	{
		if (const TSharedPtr<FMacTransportState, ESPMode::ThreadSafe> PinnedOwner = Owner.Pin())
		{
			PinnedOwner->RemoveActiveRequest(Request.RequestId, this);
		}
	}
}

void FMacRequestOperation::InvalidateNativeObjectsBeforeAdmission()
{
	dispatch_source_t LocalMonitor = nullptr;
	FUnrealAIMacUrlSessionDelegate *LocalDelegate = nil;
	NSURLSession *LocalSession = nil;
	bool bResumeMonitorBeforeCancel = false;
	{
		FScopeLock Lock(&Mutex);
		check(!bNativeSourcesActivated);
		bAbandonedBeforeAdmission = true;
		bPrepared = false;
		bNativeInvalidationStarted = true;
		LocalMonitor = Monitor;
		Monitor = nullptr;
		LocalDelegate = NativeDelegate;
		NativeDelegate = nil;
		bResumeMonitorBeforeCancel = LocalMonitor != nullptr && !bNativeSourcesActivated;
		LocalSession = Session;
		Session = nil;
		Task = nil;
		EventSink.Reset();
	}
	if (LocalDelegate != nil)
	{
		LocalDelegate->CallbackState->ClearOperation();
	}
	if (LocalMonitor != nullptr)
	{
		dispatch_source_set_event_handler(LocalMonitor, ^{
										  });
		if (bResumeMonitorBeforeCancel)
		{
			dispatch_resume(LocalMonitor);
		}
		dispatch_source_cancel(LocalMonitor);
	}
	if (LocalSession != nil)
	{
		[LocalSession invalidateAndCancel];
	}
}

bool FMacTransportState::StartRequest(
	const FUnrealAIHttpRequest &Request,
	const TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> &AccessContext,
	const TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> &EventSink,
	const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle, FString &OutError)
{
	OutHandle.Reset();
	OutError.Reset();
	FString ShapeError;
	if (!Options.ValidateShape(ShapeError) || !Request.ValidateShape(ShapeError))
	{
		OutError = ShapeError;
		return false;
	}
	if (!AccessContext->IsValid() || !AccessContext->RequiresCredential())
	{
		OutError = TEXT("Credentialed HTTPS requests require a valid credentialed access context.");
		return false;
	}
	if (Cancellation.IsCancellationRequested())
	{
		OutError = TEXT("Credentialed HTTPS request was cancelled before admission.");
		return false;
	}

	const FGuid RequestGuid = Request.RequestId.Value;
	bool bSuspendNativeTaskForTesting = false;
	bool bEmitProvisionalInvalidationBeforeCommitForTesting = false;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	bSuspendNativeTaskForTesting = Options.bSuspendNativeTaskForTesting;
	bEmitProvisionalInvalidationBeforeCommitForTesting = Options.bEmitProvisionalInvalidationBeforeCommitForTesting;
#endif

	// Phase one reserves exact identity and capacity only. No clock, access,
	// credential, provider, native, cancellation, or callback seam is entered
	// while the state lock is held.
	{
		FScopeLock Lock(&Mutex);
		if (bShuttingDown)
		{
			OutError = TEXT("Credentialed HTTPS transport is shutting down.");
			return false;
		}
		if (ReservedRequestIds.Num() + ActiveRequests.Num() >= Options.MaxActiveRequests)
		{
			OutError = TEXT("Credentialed HTTPS transport has reached its active request limit.");
			return false;
		}
		if (ReservedRequestIds.Contains(RequestGuid) || ActiveRequests.Contains(RequestGuid))
		{
			OutError = TEXT("Credentialed HTTPS request ID is already active.");
			return false;
		}
		ReservedRequestIds.Add(RequestGuid);
	}

	// Construction evaluates the injected clock. It deliberately occurs after
	// capacity reservation and outside the state lock so even a reentrant test
	// clock cannot deadlock PumpDeadlines or BeginShutdown.
	TSharedRef<FMacRequestOperation, ESPMode::ThreadSafe> Operation =
		MakeShared<FMacRequestOperation, ESPMode::ThreadSafe>(
			Request, EventSink, Cancellation, AsShared(), Options.Clock.ToSharedRef(), Options.CancellationPollSeconds,
			bSuspendNativeTaskForTesting);

	bool bAttached = false;
	{
		FScopeLock Lock(&Mutex);
		if (!bShuttingDown && ReservedRequestIds.Contains(RequestGuid) && !ActiveRequests.Contains(RequestGuid))
		{
			ReservedRequestIds.Remove(RequestGuid);
			ActiveRequests.Add(RequestGuid, Operation);
			bAttached = true;
		}
		else
		{
			ReservedRequestIds.Remove(RequestGuid);
		}
	}
	if (!bAttached)
	{
		Operation->AbandonBeforeAdmission();
		OutError = TEXT("Credentialed HTTPS request admission closed before native preparation.");
		return false;
	}

	bool bMayDispatch = false;
	{
		FScopeLock Lock(&Mutex);
		const TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> *Found = ActiveRequests.Find(RequestGuid);
		bMayDispatch = !bShuttingDown && Found != nullptr && Found->Get() == &Operation.Get();
	}

	bool bDispatched = false;
	if (bMayDispatch && !Cancellation.IsCancellationRequested())
	{
		FString DispatchError;
		bDispatched = AccessContext->TryDispatch(*Operation, DispatchError) && Operation->IsPrepared();
	}
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	if (bDispatched && bEmitProvisionalInvalidationBeforeCommitForTesting)
	{
		// Exercise an exact native invalidation callback after complete
		// preparation but before the state commit. The synchronized delegate
		// portal must stage it for exact replay once admission commits.
		const bool bInvalidationEmitted = Operation->EmitProvisionalInvalidationForTesting();
		check(bInvalidationEmitted);
		bDispatched = bInvalidationEmitted;
	}
#endif

	// Phase two is the true-admission linearization point. Shutdown either
	// observes this exact operation as committed and cancels it normally, or
	// removes/abandons it as provisional so StartRequest returns false with no
	// callback source authorized to enter the sink.
	bool bCommitted = false;
	FMacAdmissionCommitPlan CommitPlan;
	{
		FScopeLock Lock(&Mutex);
		const TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> *Found = ActiveRequests.Find(RequestGuid);
		if (bDispatched && !bShuttingDown && Found != nullptr && Found->Get() == &Operation.Get() &&
			Operation->ReserveAdmissionCommit(CommitPlan))
		{
			CommittedRequestIds.Add(RequestGuid);
			bCommitted = true;
		}
		else if (Found != nullptr && Found->Get() == &Operation.Get())
		{
			CommittedRequestIds.Remove(RequestGuid);
			ActiveRequests.Remove(RequestGuid);
		}
	}
	if (!bCommitted)
	{
		Operation->AbandonBeforeAdmission();
		OutError = TEXT("Credentialed HTTPS request admission failed or closed during preparation.");
		return false;
	}

#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	if (Options.AfterAdmissionCommitReservedForTesting.IsValid())
	{
		(*Options.AfterAdmissionCommitReservedForTesting)();
	}
#endif
	Operation->CompleteAdmissionCommit(MoveTemp(CommitPlan));
	OutHandle = MakeShared<FMacRequestHandle, ESPMode::ThreadSafe>(Operation);
	return true;
}

void FMacTransportState::PumpDeadlines()
{
	TArray<TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe>> Snapshot;
	{
		FScopeLock Lock(&Mutex);
		Snapshot.Reserve(CommittedRequestIds.Num());
		for (const FGuid &RequestGuid : CommittedRequestIds)
		{
			if (const TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> *Found = ActiveRequests.Find(RequestGuid))
			{
				Snapshot.Add(*Found);
			}
		}
	}
	for (const TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> &Operation : Snapshot)
	{
		if (Operation.IsValid())
		{
			Operation->PollCancellationAndDeadline();
		}
	}
}

void FMacTransportState::BeginShutdown()
{
	TArray<TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe>> CommittedSnapshot;
	TArray<TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe>> ProvisionalSnapshot;
	{
		FScopeLock Lock(&Mutex);
		if (bShuttingDown)
		{
			return;
		}
		bShuttingDown = true;
		ReservedRequestIds.Reset();
		TArray<FGuid> ProvisionalIds;
		for (const TPair<FGuid, TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe>> &Pair : ActiveRequests)
		{
			if (CommittedRequestIds.Contains(Pair.Key))
			{
				CommittedSnapshot.Add(Pair.Value);
			}
			else
			{
				ProvisionalIds.Add(Pair.Key);
				ProvisionalSnapshot.Add(Pair.Value);
			}
		}
		for (const FGuid &RequestGuid : ProvisionalIds)
		{
			ActiveRequests.Remove(RequestGuid);
		}
	}
	for (const TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> &Operation : ProvisionalSnapshot)
	{
		if (Operation.IsValid())
		{
			Operation->AbandonBeforeAdmission();
		}
	}
	for (const TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> &Operation : CommittedSnapshot)
	{
		if (Operation.IsValid())
		{
			Operation->CancelForShutdown();
		}
	}
}

void FMacTransportState::RemoveActiveRequest(const FUnrealAIRequestId &RequestId,
											 const FMacRequestOperation *ExpectedOperation)
{
	FScopeLock Lock(&Mutex);
	const TSharedPtr<FMacRequestOperation, ESPMode::ThreadSafe> *Found = ActiveRequests.Find(RequestId.Value);
	if (Found != nullptr && Found->Get() == ExpectedOperation)
	{
		ActiveRequests.Remove(RequestId.Value);
		CommittedRequestIds.Remove(RequestId.Value);
	}
}

class FMacAgentHttpTransport final : public IUnrealAIHttpTransport
{
  public:
	explicit FMacAgentHttpTransport(const FUnrealAIHttpTransportOptions &Options)
		: State(MakeShared<FMacTransportState, ESPMode::ThreadSafe>(Options))
	{
	}

	~FMacAgentHttpTransport() override
	{
		State->BeginShutdown();
	}

	bool StartRequest(const FUnrealAIHttpRequest &Request,
					  const TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> &AccessContext,
					  const TSharedRef<IUnrealAIHttpEventSink, ESPMode::ThreadSafe> &EventSink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FString &OutError) override
	{
		return State->StartRequest(Request, AccessContext, EventSink, Cancellation, OutHandle, OutError);
	}

	void BeginShutdown() override
	{
		State->BeginShutdown();
	}

	void PumpDeadlines() override
	{
		State->PumpDeadlines();
	}

  private:
	TSharedRef<FMacTransportState, ESPMode::ThreadSafe> State;
};

TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe>
CreateMacUrlSessionTransport(const FUnrealAIHttpTransportOptions &Options)
{
	return MakeShared<FMacAgentHttpTransport, ESPMode::ThreadSafe>(Options);
}
} // namespace UE::UnrealAI::Transport::Private

#endif // PLATFORM_MAC
