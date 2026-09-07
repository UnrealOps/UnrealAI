// Copyright UnrealOps. All Rights Reserved.

// CoreServices declares its own FVector. Match Unreal's Mac system-header boundary before including Core types.
#define FVector FVectorWorkaround
#import <Foundation/Foundation.h>
#undef check
#undef verify
#undef FVector

#include "Transport/UnrealAIMacOAuthHttpTransport.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

#include <dispatch/dispatch.h>

namespace UE::UnrealAI::Transport::Private
{
class FMacOAuthHttpTransportState;

void WipeOAuthBytes(TArray<uint8> &Bytes)
{
	volatile uint8 *Wipe = Bytes.GetData();
	for (int32 Index = 0; Wipe != nullptr && Index < Bytes.Max(); ++Index)
	{
		Wipe[Index] = 0;
	}
	Bytes.Empty();
}

FUnrealAIOAuthHttpError MakeOAuthHttpError(const EUnrealAIErrorCategory Category,
										   const EUnrealAIOAuthHttpErrorCode Code, const bool bRetryable = false)
{
	FUnrealAIOAuthHttpError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

EUnrealAITerminalKind ToOAuthGuardTerminal(const EUnrealAIOAuthHttpTerminalKind Kind)
{
	switch (Kind)
	{
	case EUnrealAIOAuthHttpTerminalKind::Succeeded:
		return EUnrealAITerminalKind::Succeeded;
	case EUnrealAIOAuthHttpTerminalKind::Failed:
		return EUnrealAITerminalKind::Failed;
	case EUnrealAIOAuthHttpTerminalKind::Cancelled:
		return EUnrealAITerminalKind::Cancelled;
	case EUnrealAIOAuthHttpTerminalKind::TimedOut:
		return EUnrealAITerminalKind::TimedOut;
	default:
		return EUnrealAITerminalKind::None;
	}
}

FString CopyBoundedOAuthHeaderValue(NSString *Value, const int32 MaxUtf8Bytes)
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

float ParseOAuthRetryAfterSeconds(NSString *Value)
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

NSString *MakeOAuthNSString(const FString &Value)
{
	FTCHARToUTF8 Utf8(*Value);
	return [[NSString alloc] initWithBytes:Utf8.Get()
									length:static_cast<NSUInteger>(Utf8.Length())
								  encoding:NSUTF8StringEncoding];
}

class FMacOAuthHttpRequestOperation final : public IUnrealAIOAuthHttpSecretConsumer,
											public TSharedFromThis<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>
{
  public:
	FMacOAuthHttpRequestOperation(FUnrealAIOAuthHttpRequest &&InRequest,
								  TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> InCompletionSink,
								  FUnrealAICancellationToken InCancellation,
								  TWeakPtr<FMacOAuthHttpTransportState, ESPMode::ThreadSafe> InOwner,
								  double InCancellationPollSeconds, bool bInSuspendNativeTaskForTesting);
	~FMacOAuthHttpRequestOperation() override;

	FUnrealAIRequestId GetRequestId() const;
	bool PrepareAndStart();
	void AbandonBeforeAdmission();
	void CancelByCaller();
	void CancelForShutdown();
	void OnRedirectRejected();
	void OnAuthenticationChallengeRejected();
	bool OnResponseStarted(const FUnrealAIOAuthHttpResponseMetadata &Metadata);
	enum class EBodyResult : uint8
	{
		Accepted,
		Ignored,
		TooLarge,
		InvalidSequence
	};
	EBodyResult OnBodyData(const uint8 *Bytes, int64 NumBytes);
	void OnResponseTooLarge();
	void OnInvalidResponse();
	void OnTaskCompleted(NSError *Error);

  protected:
	bool ConsumeOAuthHttpSecret(TConstArrayView<uint8> Secret) override;

  private:
	bool PrepareNativeRequest(TConstArrayView<uint8> Secret);
	void PollCancellationAndDeadline();
	void Finish(EUnrealAIOAuthHttpTerminalKind Kind, const FUnrealAIOAuthHttpError &Error);
	void QueueCompletion(FUnrealAIOAuthHttpResult &&Result);
	void InvalidateNativeObjects(bool bCancel);

	mutable FCriticalSection Mutex;
	FUnrealAIOAuthHttpRequest Request;
	TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> CompletionSink;
	FUnrealAICancellationToken Cancellation;
	TWeakPtr<FMacOAuthHttpTransportState, ESPMode::ThreadSafe> Owner;
	FUnrealAITerminalGuard TerminalGuard;
	FUnrealAIOAuthHttpResponseMetadata ResponseMetadata;
	TArray<uint8> ResponseBytes;
	bool bResponseStarted = false;
	bool bPrepared = false;
	double DeadlineSeconds = 0.0;
	double CancellationPollSeconds = 0.05;
	bool bSuspendNativeTaskForTesting = false;
	dispatch_queue_t CompletionQueue = nullptr;
	dispatch_source_t Monitor = nullptr;
	NSURLSession *Session = nil;
	NSURLSessionDataTask *Task = nil;
};

class FMacOAuthHttpTransportState final : public TSharedFromThis<FMacOAuthHttpTransportState, ESPMode::ThreadSafe>
{
  public:
	explicit FMacOAuthHttpTransportState(FUnrealAIOAuthHttpTransportOptions InOptions) : Options(MoveTemp(InOptions)) {}

	bool StartRequest(FUnrealAIOAuthHttpRequest &&Request,
					  const TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> &CompletionSink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle, FString &OutError);
	void BeginShutdown();
	void RemoveActiveRequest(const FUnrealAIRequestId &RequestId,
							 const FMacOAuthHttpRequestOperation *ExpectedOperation);

  private:
	FCriticalSection Mutex;
	FUnrealAIOAuthHttpTransportOptions Options;
	TMap<FGuid, TSharedPtr<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>> ActiveRequests;
	bool bShuttingDown = false;
};
} // namespace UE::UnrealAI::Transport::Private

@interface FAgentMacOAuthUrlSessionDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
{
  @public
	TSharedPtr<UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> Operation;
}
@end

@implementation FAgentMacOAuthUrlSessionDelegate
- (void)URLSession:(NSURLSession *)Session
						  task:(NSURLSessionTask *)Task
	willPerformHTTPRedirection:(NSHTTPURLResponse *)Response
					newRequest:(NSURLRequest *)Request
			 completionHandler:(void (^)(NSURLRequest *_Nullable))CompletionHandler
{
	CompletionHandler(nil);
	const TSharedPtr<UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>
		LocalOperation = Operation;
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
	const TSharedPtr<UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>
		LocalOperation = Operation;
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
	const TSharedPtr<UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>
		LocalOperation = Operation;
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
	const TSharedPtr<UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>
		LocalOperation = Operation;
	NSHTTPURLResponse *HttpResponse =
		[Response isKindOfClass:[NSHTTPURLResponse class]] ? static_cast<NSHTTPURLResponse *>(Response) : nil;
	if (!LocalOperation.IsValid() || HttpResponse == nil)
	{
		CompletionHandler(NSURLSessionResponseCancel);
		if (LocalOperation.IsValid())
		{
			LocalOperation->OnInvalidResponse();
		}
		return;
	}

	FUnrealAIOAuthHttpResponseMetadata Metadata;
	Metadata.StatusCode = static_cast<int32>(HttpResponse.statusCode);
	Metadata.ContentType = UE::UnrealAI::Transport::Private::CopyBoundedOAuthHeaderValue(
		[HttpResponse valueForHTTPHeaderField:@"Content-Type"],
		FUnrealAIOAuthHttpResponseMetadata::MaxContentTypeBytes);
	Metadata.ProviderRequestId = UE::UnrealAI::Transport::Private::CopyBoundedOAuthHeaderValue(
		[HttpResponse valueForHTTPHeaderField:@"X-Request-ID"],
		FUnrealAIOAuthHttpResponseMetadata::MaxProviderRequestIdBytes);
	Metadata.RetryAfterSeconds = UE::UnrealAI::Transport::Private::ParseOAuthRetryAfterSeconds(
		[HttpResponse valueForHTTPHeaderField:@"Retry-After"]);
	CompletionHandler(LocalOperation->OnResponseStarted(Metadata) ? NSURLSessionResponseAllow
																  : NSURLSessionResponseCancel);
}

- (void)URLSession:(NSURLSession *)Session dataTask:(NSURLSessionDataTask *)DataTask didReceiveData:(NSData *)Data
{
	const TSharedPtr<UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>
		LocalOperation = Operation;
	if (!LocalOperation.IsValid())
	{
		return;
	}
	const NSUInteger NativeLength = Data.length;
	if (NativeLength > static_cast<NSUInteger>(MAX_int64))
	{
		LocalOperation->OnResponseTooLarge();
		return;
	}
	const auto Result =
		LocalOperation->OnBodyData(static_cast<const uint8 *>(Data.bytes), static_cast<int64>(NativeLength));
	if (Result == UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation::EBodyResult::TooLarge)
	{
		LocalOperation->OnResponseTooLarge();
	}
	else if (Result == UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation::EBodyResult::InvalidSequence)
	{
		LocalOperation->OnInvalidResponse();
	}
}

- (void)URLSession:(NSURLSession *)Session task:(NSURLSessionTask *)Task didCompleteWithError:(NSError *)Error
{
	const TSharedPtr<UE::UnrealAI::Transport::Private::FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>
		LocalOperation = Operation;
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
	Operation.Reset();
}
@end

namespace UE::UnrealAI::Transport::Private
{
class FMacOAuthHttpRequestHandle final : public IUnrealAIOAuthHttpRequestHandle
{
  public:
	explicit FMacOAuthHttpRequestHandle(
		const TSharedRef<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> &InOperation)
		: RequestId(InOperation->GetRequestId()), Operation(InOperation)
	{
	}

	FUnrealAIRequestId GetRequestId() const override
	{
		return RequestId;
	}

	void Cancel() override
	{
		if (const TSharedPtr<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> Pinned = Operation.Pin())
		{
			Pinned->CancelByCaller();
		}
	}

  private:
	FUnrealAIRequestId RequestId;
	TWeakPtr<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> Operation;
};

FMacOAuthHttpRequestOperation::FMacOAuthHttpRequestOperation(
	FUnrealAIOAuthHttpRequest &&InRequest,
	TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> InCompletionSink,
	FUnrealAICancellationToken InCancellation, TWeakPtr<FMacOAuthHttpTransportState, ESPMode::ThreadSafe> InOwner,
	const double InCancellationPollSeconds, const bool bInSuspendNativeTaskForTesting)
	: Request(MoveTemp(InRequest)), CompletionSink(MoveTemp(InCompletionSink)), Cancellation(MoveTemp(InCancellation)),
	  Owner(MoveTemp(InOwner)), DeadlineSeconds(FPlatformTime::Seconds() + Request.GetTimeoutSeconds()),
	  CancellationPollSeconds(InCancellationPollSeconds), bSuspendNativeTaskForTesting(bInSuspendNativeTaskForTesting)
{
	CompletionQueue =
		dispatch_queue_create("com.unrealops.unrealai.oauth-transport.completion", DISPATCH_QUEUE_SERIAL);
}

FMacOAuthHttpRequestOperation::~FMacOAuthHttpRequestOperation()
{
	if (Monitor != nullptr)
	{
		dispatch_source_cancel(Monitor);
		Monitor = nullptr;
	}
	WipeOAuthBytes(ResponseBytes);
}

FUnrealAIRequestId FMacOAuthHttpRequestOperation::GetRequestId() const
{
	return Request.GetRequestId();
}

bool FMacOAuthHttpRequestOperation::PrepareAndStart()
{
	if (Request.GetMethod() == EUnrealAIOAuthHttpMethod::Get)
	{
		return PrepareNativeRequest(TConstArrayView<uint8>());
	}
	return Request.TryConsumeBody(*this);
}

bool FMacOAuthHttpRequestOperation::ConsumeOAuthHttpSecret(const TConstArrayView<uint8> Secret)
{
	return PrepareNativeRequest(Secret);
}

bool FMacOAuthHttpRequestOperation::PrepareNativeRequest(const TConstArrayView<uint8> Secret)
{
	const bool bValidGet = Request.GetMethod() == EUnrealAIOAuthHttpMethod::Get && Secret.IsEmpty();
	const bool bValidPost = Request.GetMethod() == EUnrealAIOAuthHttpMethod::Post && !Secret.IsEmpty() &&
							Secret.Num() <= FUnrealAISecretValue::MaxSecretBytes;
	if ((!bValidGet && !bValidPost) || Cancellation.IsCancellationRequested() ||
		FPlatformTime::Seconds() >= DeadlineSeconds)
	{
		return false;
	}

	@autoreleasepool
	{
		NSString *Origin = MakeOAuthNSString(Request.GetEndpointOrigin().ToString());
		NSString *Path = MakeOAuthNSString(Request.GetRelativePath());
		if (Origin == nil || Path == nil)
		{
			return false;
		}
		NSURLComponents *Components = [NSURLComponents componentsWithString:Origin];
		if (Components == nil || ![Components.scheme isEqualToString:@"https"] || Components.host.length == 0 ||
			Components.user != nil || Components.password != nil || Components.query != nil ||
			Components.fragment != nil ||
			(Components.percentEncodedPath.length != 0 && ![Components.percentEncodedPath isEqualToString:@"/"]))
		{
			return false;
		}
		Components.percentEncodedPath = Path;
		Components.percentEncodedQuery = nil;
		Components.fragment = nil;
		NSURL *Url = Components.URL;
		if (Url == nil || ![Url.scheme isEqualToString:@"https"] || Url.host.length == 0 || Url.user != nil ||
			Url.password != nil || Url.query != nil || Url.fragment != nil)
		{
			return false;
		}

		NSMutableURLRequest *NativeRequest = [[NSMutableURLRequest alloc] initWithURL:Url];
		if (NativeRequest == nil)
		{
			return false;
		}
		NativeRequest.HTTPMethod = bValidGet ? @"GET" : @"POST";
		NativeRequest.timeoutInterval = Request.GetTimeoutSeconds();
		NativeRequest.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
		NativeRequest.HTTPShouldHandleCookies = NO;
		[NativeRequest setValue:@"application/json" forHTTPHeaderField:@"Accept"];
		if (bValidPost)
		{
			switch (Request.GetContentType())
			{
			case EUnrealAIOAuthHttpContentType::ApplicationJson:
				[NativeRequest setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
				break;
			case EUnrealAIOAuthHttpContentType::FormUrlEncoded:
				[NativeRequest setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
				break;
			default:
				return false;
			}

			NSMutableData *BodyData = [NSMutableData dataWithLength:static_cast<NSUInteger>(Secret.Num())];
			if (BodyData == nil || BodyData.mutableBytes == nullptr)
			{
				return false;
			}
			FMemory::Memcpy(BodyData.mutableBytes, Secret.GetData(), static_cast<SIZE_T>(Secret.Num()));
			NativeRequest.HTTPBody = BodyData;
			FMemory::Memzero(BodyData.mutableBytes, static_cast<SIZE_T>(Secret.Num()));
		}

		NSURLSessionConfiguration *Configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		if (Configuration == nil)
		{
			return false;
		}
		Configuration.URLCache = nil;
		Configuration.HTTPCookieStorage = nil;
		Configuration.URLCredentialStorage = nil;
		Configuration.connectionProxyDictionary = @{};
		Configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
		Configuration.HTTPShouldSetCookies = NO;
		Configuration.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
		Configuration.timeoutIntervalForRequest = Request.GetTimeoutSeconds();
		Configuration.timeoutIntervalForResource = Request.GetTimeoutSeconds();
		Configuration.TLSMinimumSupportedProtocolVersion = tls_protocol_version_TLSv12;
		Configuration.waitsForConnectivity = NO;

		FAgentMacOAuthUrlSessionDelegate *Delegate = [[FAgentMacOAuthUrlSessionDelegate alloc] init];
		NSOperationQueue *DelegateQueue = [[NSOperationQueue alloc] init];
		if (Delegate == nil || DelegateQueue == nil)
		{
			return false;
		}
		Delegate->Operation = AsShared();
		DelegateQueue.maxConcurrentOperationCount = 1;
		DelegateQueue.name = @"com.unrealops.unrealai.oauth-transport.delegate";
		NSURLSession *NewSession = [NSURLSession sessionWithConfiguration:Configuration
																 delegate:Delegate
															delegateQueue:DelegateQueue];
		NSURLSessionDataTask *NewTask = [NewSession dataTaskWithRequest:NativeRequest];
		if (NewSession == nil || NewTask == nil)
		{
			[NewSession invalidateAndCancel];
			return false;
		}

		TWeakPtr<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> WeakSelf = AsShared();
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
		  if (const TSharedPtr<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> Pinned = WeakSelf.Pin())
		  {
			  Pinned->PollCancellationAndDeadline();
		  }
		});

		{
			FScopeLock Lock(&Mutex);
			if (bPrepared || TerminalGuard.IsComplete() || Cancellation.IsCancellationRequested() ||
				FPlatformTime::Seconds() >= DeadlineSeconds)
			{
				dispatch_resume(NewMonitor);
				dispatch_source_cancel(NewMonitor);
				[NewSession invalidateAndCancel];
				return false;
			}
			Session = NewSession;
			Task = NewTask;
			Monitor = NewMonitor;
			bPrepared = true;
		}
		dispatch_resume(NewMonitor);
		if (!bSuspendNativeTaskForTesting)
		{
			[NewTask resume];
		}
		return true;
	}
}

void FMacOAuthHttpRequestOperation::PollCancellationAndDeadline()
{
	if (Cancellation.IsCancellationRequested())
	{
		if (Cancellation.GetReason() == EUnrealAICancellationReason::Timeout)
		{
			Finish(EUnrealAIOAuthHttpTerminalKind::TimedOut,
				   MakeOAuthHttpError(EUnrealAIErrorCategory::Timeout, EUnrealAIOAuthHttpErrorCode::TimedOut, true));
		}
		else
		{
			Finish(EUnrealAIOAuthHttpTerminalKind::Cancelled,
				   MakeOAuthHttpError(EUnrealAIErrorCategory::Cancelled, EUnrealAIOAuthHttpErrorCode::Cancelled));
		}
		return;
	}
	if (FPlatformTime::Seconds() >= DeadlineSeconds)
	{
		Finish(EUnrealAIOAuthHttpTerminalKind::TimedOut,
			   MakeOAuthHttpError(EUnrealAIErrorCategory::Timeout, EUnrealAIOAuthHttpErrorCode::TimedOut, true));
	}
}

void FMacOAuthHttpRequestOperation::AbandonBeforeAdmission()
{
	InvalidateNativeObjects(true);
}

void FMacOAuthHttpRequestOperation::CancelByCaller()
{
	Finish(EUnrealAIOAuthHttpTerminalKind::Cancelled,
		   MakeOAuthHttpError(EUnrealAIErrorCategory::Cancelled, EUnrealAIOAuthHttpErrorCode::Cancelled));
}

void FMacOAuthHttpRequestOperation::CancelForShutdown()
{
	Finish(EUnrealAIOAuthHttpTerminalKind::Cancelled,
		   MakeOAuthHttpError(EUnrealAIErrorCategory::Cancelled, EUnrealAIOAuthHttpErrorCode::Cancelled));
}

void FMacOAuthHttpRequestOperation::OnRedirectRejected()
{
	Finish(EUnrealAIOAuthHttpTerminalKind::Failed,
		   MakeOAuthHttpError(EUnrealAIErrorCategory::Transport, EUnrealAIOAuthHttpErrorCode::RedirectRejected));
}

void FMacOAuthHttpRequestOperation::OnAuthenticationChallengeRejected()
{
	Finish(EUnrealAIOAuthHttpTerminalKind::Failed,
		   MakeOAuthHttpError(EUnrealAIErrorCategory::Transport,
							  EUnrealAIOAuthHttpErrorCode::AuthenticationChallengeRejected));
}

bool FMacOAuthHttpRequestOperation::OnResponseStarted(const FUnrealAIOAuthHttpResponseMetadata &Metadata)
{
	FString ShapeError;
	if (!Metadata.ValidateShape(ShapeError))
	{
		OnInvalidResponse();
		return false;
	}

	FScopeLock Lock(&Mutex);
	if (TerminalGuard.IsComplete() || bResponseStarted)
	{
		return false;
	}
	bResponseStarted = true;
	ResponseMetadata = Metadata;
	ResponseBytes.Reserve(Request.GetMaxResponseBodyBytes());
	return true;
}

FMacOAuthHttpRequestOperation::EBodyResult FMacOAuthHttpRequestOperation::OnBodyData(const uint8 *Bytes,
																					 const int64 NumBytes)
{
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
	if (NumBytes > static_cast<int64>(Request.GetMaxResponseBodyBytes() - ResponseBytes.Num()))
	{
		return EBodyResult::TooLarge;
	}
	ResponseBytes.Append(Bytes, static_cast<int32>(NumBytes));
	return EBodyResult::Accepted;
}

void FMacOAuthHttpRequestOperation::OnResponseTooLarge()
{
	Finish(EUnrealAIOAuthHttpTerminalKind::Failed,
		   MakeOAuthHttpError(EUnrealAIErrorCategory::Transport, EUnrealAIOAuthHttpErrorCode::ResponseTooLarge));
}

void FMacOAuthHttpRequestOperation::OnInvalidResponse()
{
	Finish(EUnrealAIOAuthHttpTerminalKind::Failed,
		   MakeOAuthHttpError(EUnrealAIErrorCategory::ProviderProtocol, EUnrealAIOAuthHttpErrorCode::InvalidResponse));
}

void FMacOAuthHttpRequestOperation::OnTaskCompleted(NSError *Error)
{
	if (Error == nil)
	{
		bool bHasResponse = false;
		{
			FScopeLock Lock(&Mutex);
			bHasResponse = bResponseStarted;
		}
		if (!bHasResponse)
		{
			OnInvalidResponse();
			return;
		}
		Finish(EUnrealAIOAuthHttpTerminalKind::Succeeded, FUnrealAIOAuthHttpError());
		return;
	}

	if ([Error.domain isEqualToString:NSURLErrorDomain] && Error.code == NSURLErrorTimedOut)
	{
		Finish(EUnrealAIOAuthHttpTerminalKind::TimedOut,
			   MakeOAuthHttpError(EUnrealAIErrorCategory::Timeout, EUnrealAIOAuthHttpErrorCode::TimedOut, true));
		return;
	}
	if ([Error.domain isEqualToString:NSURLErrorDomain] && Error.code == NSURLErrorDataLengthExceedsMaximum)
	{
		OnResponseTooLarge();
		return;
	}
	if ([Error.domain isEqualToString:NSURLErrorDomain] && Error.code == NSURLErrorBadServerResponse)
	{
		OnInvalidResponse();
		return;
	}

	Finish(EUnrealAIOAuthHttpTerminalKind::Failed,
		   MakeOAuthHttpError(EUnrealAIErrorCategory::Transport, EUnrealAIOAuthHttpErrorCode::TransportFailed, true));
}

void FMacOAuthHttpRequestOperation::Finish(const EUnrealAIOAuthHttpTerminalKind Kind,
										   const FUnrealAIOAuthHttpError &Error)
{
	const EUnrealAITerminalKind GuardKind = ToOAuthGuardTerminal(Kind);
	if (GuardKind == EUnrealAITerminalKind::None)
	{
		return;
	}

	dispatch_source_t LocalMonitor = nullptr;
	FUnrealAIOAuthHttpResponseMetadata LocalMetadata;
	TArray<uint8> LocalResponseBytes;
	{
		FScopeLock Lock(&Mutex);
		if (!TerminalGuard.TryComplete(GuardKind))
		{
			return;
		}
		LocalMonitor = Monitor;
		Monitor = nullptr;
		if (Kind == EUnrealAIOAuthHttpTerminalKind::Succeeded)
		{
			LocalMetadata = ResponseMetadata;
			LocalResponseBytes = MoveTemp(ResponseBytes);
		}
		else
		{
			WipeOAuthBytes(ResponseBytes);
		}
	}

	if (LocalMonitor != nullptr)
	{
		dispatch_source_cancel(LocalMonitor);
	}

	FUnrealAIOAuthHttpResult Result;
	FString ResultError;
	bool bResultCreated = false;
	if (Kind == EUnrealAIOAuthHttpTerminalKind::Succeeded)
	{
		FUnrealAISecretValue ResponseSecret;
		if (!LocalResponseBytes.IsEmpty())
		{
			bResultCreated =
				FUnrealAISecretValue::TryCreate(MoveTemp(LocalResponseBytes), ResponseSecret, ResultError) &&
				FUnrealAIOAuthHttpResult::TryCreateResponse(Request.GetRequestId(), LocalMetadata,
															MoveTemp(ResponseSecret), Result, ResultError);
		}
		else
		{
			bResultCreated = FUnrealAIOAuthHttpResult::TryCreateResponse(Request.GetRequestId(), LocalMetadata,
																		 MoveTemp(ResponseSecret), Result, ResultError);
		}
		if (!bResultCreated)
		{
			WipeOAuthBytes(LocalResponseBytes);
			const FUnrealAIOAuthHttpError InvalidResponseError = MakeOAuthHttpError(
				EUnrealAIErrorCategory::ProviderProtocol, EUnrealAIOAuthHttpErrorCode::InvalidResponse);
			bResultCreated =
				FUnrealAIOAuthHttpResult::TryCreateError(Request.GetRequestId(), EUnrealAIOAuthHttpTerminalKind::Failed,
														 InvalidResponseError, Result, ResultError);
		}
	}
	else
	{
		bResultCreated =
			FUnrealAIOAuthHttpResult::TryCreateError(Request.GetRequestId(), Kind, Error, Result, ResultError);
	}
	check(bResultCreated);
	if (bResultCreated)
	{
		QueueCompletion(MoveTemp(Result));
	}

	InvalidateNativeObjects(Kind != EUnrealAIOAuthHttpTerminalKind::Succeeded);
	if (const TSharedPtr<FMacOAuthHttpTransportState, ESPMode::ThreadSafe> PinnedOwner = Owner.Pin())
	{
		PinnedOwner->RemoveActiveRequest(Request.GetRequestId(), this);
	}
}

void FMacOAuthHttpRequestOperation::QueueCompletion(FUnrealAIOAuthHttpResult &&Result)
{
	TSharedRef<FUnrealAIOAuthHttpResult, ESPMode::ThreadSafe> QueuedResult =
		MakeShared<FUnrealAIOAuthHttpResult, ESPMode::ThreadSafe>(MoveTemp(Result));
	TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> Sink = CompletionSink;
	dispatch_async(CompletionQueue, ^{
	  Sink->CompleteOAuthHttpRequest(MoveTemp(*QueuedResult));
	});
}

void FMacOAuthHttpRequestOperation::InvalidateNativeObjects(const bool bCancel)
{
	NSURLSession *LocalSession = nil;
	NSURLSessionDataTask *LocalTask = nil;
	{
		FScopeLock Lock(&Mutex);
		LocalSession = Session;
		LocalTask = Task;
		Session = nil;
		Task = nil;
	}
	if (bCancel)
	{
		[LocalTask cancel];
		[LocalSession invalidateAndCancel];
	}
	else
	{
		[LocalSession finishTasksAndInvalidate];
	}
}

bool FMacOAuthHttpTransportState::StartRequest(
	FUnrealAIOAuthHttpRequest &&Request,
	const TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> &CompletionSink,
	const FUnrealAICancellationToken &Cancellation,
	TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle, FString &OutError)
{
	OutHandle.Reset();
	OutError.Reset();
	FString ShapeError;
	if (!Options.ValidateShape(ShapeError) || !Request.ValidateShape(ShapeError))
	{
		OutError = ShapeError;
		return false;
	}
	if (Cancellation.IsCancellationRequested())
	{
		OutError = TEXT("OAuth HTTPS request was cancelled before admission.");
		return false;
	}

	const FUnrealAIRequestId RequestId = Request.GetRequestId();
	bool bSuspendNativeTaskForTesting = false;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	bSuspendNativeTaskForTesting = Options.bSuspendNativeTaskForTesting;
#endif
	const TSharedRef<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> Operation =
		MakeShared<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>(MoveTemp(Request), CompletionSink, Cancellation,
																	   AsShared(), Options.CancellationPollSeconds,
																	   bSuspendNativeTaskForTesting);

	bool bStarted = false;
	{
		FScopeLock Lock(&Mutex);
		if (bShuttingDown)
		{
			OutError = TEXT("OAuth HTTPS transport is shutting down.");
			return false;
		}
		if (ActiveRequests.Num() >= Options.MaxActiveRequests)
		{
			OutError = TEXT("OAuth HTTPS transport has reached its active request limit.");
			return false;
		}
		if (ActiveRequests.Contains(RequestId.Value))
		{
			OutError = TEXT("OAuth HTTPS request ID is already active.");
			return false;
		}
		ActiveRequests.Add(RequestId.Value, Operation);
		bStarted = Operation->PrepareAndStart();
		if (!bStarted)
		{
			ActiveRequests.Remove(RequestId.Value);
			OutError = TEXT("OAuth HTTPS request admission failed.");
		}
	}
	if (!bStarted)
	{
		Operation->AbandonBeforeAdmission();
		return false;
	}

	OutHandle = MakeShared<FMacOAuthHttpRequestHandle, ESPMode::ThreadSafe>(Operation);
	return true;
}

void FMacOAuthHttpTransportState::BeginShutdown()
{
	TArray<TSharedPtr<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe>> Snapshot;
	{
		FScopeLock Lock(&Mutex);
		if (bShuttingDown)
		{
			return;
		}
		bShuttingDown = true;
		ActiveRequests.GenerateValueArray(Snapshot);
	}
	for (const TSharedPtr<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> &Operation : Snapshot)
	{
		if (Operation.IsValid())
		{
			Operation->CancelForShutdown();
		}
	}
}

void FMacOAuthHttpTransportState::RemoveActiveRequest(const FUnrealAIRequestId &RequestId,
													  const FMacOAuthHttpRequestOperation *ExpectedOperation)
{
	FScopeLock Lock(&Mutex);
	const TSharedPtr<FMacOAuthHttpRequestOperation, ESPMode::ThreadSafe> *Found = ActiveRequests.Find(RequestId.Value);
	if (Found != nullptr && Found->Get() == ExpectedOperation)
	{
		ActiveRequests.Remove(RequestId.Value);
	}
}

class FMacAgentOAuthHttpTransport final : public IUnrealAIOAuthHttpTransport
{
  public:
	explicit FMacAgentOAuthHttpTransport(const FUnrealAIOAuthHttpTransportOptions &Options)
		: State(MakeShared<FMacOAuthHttpTransportState, ESPMode::ThreadSafe>(Options))
	{
	}

	~FMacAgentOAuthHttpTransport() override
	{
		State->BeginShutdown();
	}

	bool StartRequest(FUnrealAIOAuthHttpRequest &&Request,
					  const TSharedRef<IUnrealAIOAuthHttpCompletionSink, ESPMode::ThreadSafe> &CompletionSink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIOAuthHttpRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FString &OutError) override
	{
		return State->StartRequest(MoveTemp(Request), CompletionSink, Cancellation, OutHandle, OutError);
	}

	void BeginShutdown() override
	{
		State->BeginShutdown();
	}

  private:
	TSharedRef<FMacOAuthHttpTransportState, ESPMode::ThreadSafe> State;
};

TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe>
CreateMacOAuthHttpTransport(const FUnrealAIOAuthHttpTransportOptions &Options)
{
	return MakeShared<FMacAgentOAuthHttpTransport, ESPMode::ThreadSafe>(Options);
}
} // namespace UE::UnrealAI::Transport::Private
