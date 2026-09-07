// Copyright UnrealOps. All Rights Reserved.

// This guard precedes Unreal headers, so PLATFORM_MAC may be undefined.
#if defined(PLATFORM_MAC) && PLATFORM_MAC

// CoreServices declares its own FVector. Match Unreal's Mac system-header boundary before including Core types.
#define FVector FVectorWorkaround
#import <Foundation/Foundation.h>
#undef check
#undef verify
#undef FVector

#include "Transport/UnrealAIMacOAuthIssuerHttpClient.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

namespace
{
FUnrealAIProviderAccessError MakeHttpError(const EUnrealAIErrorCategory Category,
										   const EUnrealAIProviderAccessErrorCode Code, const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

NSString *MakeNSString(const FString &Value)
{
	FTCHARToUTF8 Utf8(*Value);
	return [[NSString alloc] initWithBytes:Utf8.Get()
									length:static_cast<NSUInteger>(Utf8.Length())
								  encoding:NSUTF8StringEncoding];
}

bool CopyBoundedNSString(NSString *Value, const int32 MaxBytes, FString &OutValue)
{
	OutValue.Reset();
	if (Value == nil)
	{
		return true;
	}
	const char *Utf8 = [Value UTF8String];
	if (Utf8 == nullptr)
	{
		return false;
	}
	const int32 Length = FCStringAnsi::Strlen(Utf8);
	if (Length < 0 || Length > MaxBytes || [Value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] != Length)
	{
		return false;
	}
	for (int32 Index = 0; Index < Length; ++Index)
	{
		if (Utf8[Index] == '\r' || Utf8[Index] == '\n' || Utf8[Index] == '\0')
		{
			return false;
		}
	}
	OutValue = UTF8_TO_TCHAR(Utf8);
	return true;
}

class FMacIssuerRequestState final
{
  public:
	FMacIssuerRequestState(const FString &InExactUrl, const int32 InMaxBodyBytes)
		: ExactUrl(InExactUrl), MaxBodyBytes(InMaxBodyBytes), Wake(FPlatformProcess::GetSynchEventFromPool(false))
	{
	}

	~FMacIssuerRequestState()
	{
		if (Body.GetData() != nullptr && Body.Max() > 0)
		{
			FMemory::Memzero(Body.GetData(), Body.Max());
		}
		Body.Empty();
		if (Wake != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(Wake);
			Wake = nullptr;
		}
	}

	void Fail()
	{
		FScopeLock Lock(&Mutex);
		bFailed = true;
		Wake->Trigger();
	}

	void ReceiveResponse(NSHTTPURLResponse *Response)
	{
		FScopeLock Lock(&Mutex);
		if (Response == nil)
		{
			bFailed = true;
			Wake->Trigger();
			return;
		}
		StatusCode = static_cast<int32>(Response.statusCode);
		FString Effective;
		FString Type;
		FString Cache;
		if (!CopyBoundedNSString(Response.URL.absoluteString, FUnrealAIOAuthTrustedAuthorizationServer::MaxUriUtf8Bytes,
								 Effective) ||
			!CopyBoundedNSString([Response valueForHTTPHeaderField:@"Content-Type"], 256, Type) ||
			!CopyBoundedNSString([Response valueForHTTPHeaderField:@"Cache-Control"], 1024, Cache) ||
			Effective != ExactUrl || Response.expectedContentLength > MaxBodyBytes)
		{
			bFailed = true;
		}
		else
		{
			EffectiveUrl = MoveTemp(Effective);
			ContentType = MoveTemp(Type);
			CacheControl = MoveTemp(Cache);
			bResponseReceived = true;
		}
		Wake->Trigger();
	}

	void ReceiveData(NSData *Data)
	{
		FScopeLock Lock(&Mutex);
		const NSUInteger Length = Data.length;
		if (bFailed || !bResponseReceived || Length > static_cast<NSUInteger>(MAX_int32) ||
			Body.Num() > MaxBodyBytes - static_cast<int32>(Length))
		{
			bFailed = true;
			Wake->Trigger();
			return;
		}
		if (Length > 0)
		{
			Body.Append(static_cast<const uint8 *>(Data.bytes), static_cast<int32>(Length));
		}
	}

	void Complete(NSError *Error)
	{
		FScopeLock Lock(&Mutex);
		bTaskCompleted = true;
		bFailed |= Error != nil;
		Wake->Trigger();
	}

	void Invalidated(NSError *Error)
	{
		FScopeLock Lock(&Mutex);
		bInvalidated = true;
		bFailed |= Error != nil;
		Wake->Trigger();
	}

	bool IsFailed() const
	{
		FScopeLock Lock(&Mutex);
		return bFailed;
	}

	bool IsInvalidated() const
	{
		FScopeLock Lock(&Mutex);
		return bInvalidated;
	}

	bool TakeResponse(FUnrealAIOAuthIssuerHttpResponse &OutResponse)
	{
		FScopeLock Lock(&Mutex);
		if (bFailed || !bTaskCompleted || !bInvalidated || !bResponseReceived)
		{
			return false;
		}
		OutResponse.StatusCode = StatusCode;
		OutResponse.ExactEffectiveUrl = MoveTemp(EffectiveUrl);
		OutResponse.ContentType = MoveTemp(ContentType);
		OutResponse.CacheControl = MoveTemp(CacheControl);
		OutResponse.Body = MoveTemp(Body);
		return true;
	}

	FEvent *GetWakeEvent() const
	{
		return Wake;
	}

  private:
	mutable FCriticalSection Mutex;
	const FString ExactUrl;
	const int32 MaxBodyBytes = 0;
	FEvent *Wake = nullptr;
	int32 StatusCode = 0;
	FString EffectiveUrl;
	FString ContentType;
	FString CacheControl;
	TArray<uint8> Body;
	bool bResponseReceived = false;
	bool bTaskCompleted = false;
	bool bInvalidated = false;
	bool bFailed = false;
};
} // namespace

@interface FAgentMacOAuthIssuerDelegate : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
{
  @public
	TSharedPtr<FMacIssuerRequestState, ESPMode::ThreadSafe> State;
}
@end

@implementation FAgentMacOAuthIssuerDelegate
- (void)URLSession:(NSURLSession *)Session
						  task:(NSURLSessionTask *)Task
	willPerformHTTPRedirection:(NSHTTPURLResponse *)Response
					newRequest:(NSURLRequest *)Request
			 completionHandler:(void (^)(NSURLRequest *_Nullable))CompletionHandler
{
	CompletionHandler(nil);
	if (State.IsValid())
	{
		State->Fail();
	}
}

- (void)URLSession:(NSURLSession *)Session
	didReceiveChallenge:(NSURLAuthenticationChallenge *)Challenge
	  completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *_Nullable))CompletionHandler
{
	if ([Challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust])
	{
		CompletionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
		return;
	}
	CompletionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
	if (State.IsValid())
	{
		State->Fail();
	}
}

- (void)URLSession:(NSURLSession *)Session
				   task:(NSURLSessionTask *)Task
	didReceiveChallenge:(NSURLAuthenticationChallenge *)Challenge
	  completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *_Nullable))CompletionHandler
{
	if ([Challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust])
	{
		CompletionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
		return;
	}
	CompletionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
	if (State.IsValid())
	{
		State->Fail();
	}
}

- (void)URLSession:(NSURLSession *)Session
			  dataTask:(NSURLSessionDataTask *)DataTask
	didReceiveResponse:(NSURLResponse *)Response
	 completionHandler:(void (^)(NSURLSessionResponseDisposition))CompletionHandler
{
	if (![Response isKindOfClass:[NSHTTPURLResponse class]] || !State.IsValid())
	{
		if (State.IsValid())
		{
			State->Fail();
		}
		CompletionHandler(NSURLSessionResponseCancel);
		return;
	}
	State->ReceiveResponse((NSHTTPURLResponse *)Response);
	CompletionHandler(State->IsFailed() ? NSURLSessionResponseCancel : NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)Session dataTask:(NSURLSessionDataTask *)DataTask didReceiveData:(NSData *)Data
{
	if (State.IsValid())
	{
		State->ReceiveData(Data);
		if (State->IsFailed())
		{
			[DataTask cancel];
		}
	}
}

- (void)URLSession:(NSURLSession *)Session task:(NSURLSessionTask *)Task didCompleteWithError:(NSError *)Error
{
	if (State.IsValid())
	{
		State->Complete(Error);
	}
	[Session finishTasksAndInvalidate];
}

- (void)URLSession:(NSURLSession *)Session didBecomeInvalidWithError:(NSError *)Error
{
	if (State.IsValid())
	{
		State->Invalidated(Error);
	}
}
@end

namespace
{
class FMacOAuthIssuerHttpClient final : public IUnrealAIOAuthIssuerHttpClient
{
  public:
	explicit FMacOAuthIssuerHttpClient(FUnrealAIOAuthIssuerHttpClientOptions InOptions) : Options(MoveTemp(InOptions))
	{
	}

	bool Execute(const FUnrealAIOAuthAuthorizationOperationContext &Context,
				 const FUnrealAIOAuthIssuerHttpRequest &Request, FUnrealAIOAuthIssuerHttpResponse &OutResponse,
				 FUnrealAIProviderAccessError &OutError) override
	{
		OutResponse.Reset();
		OutError = {};
		FString ShapeError;
		if (!Request.ValidateShape(ShapeError) || !Context.ValidateShape(ShapeError))
		{
			OutError = MakeHttpError(EUnrealAIErrorCategory::InvalidArgument,
									 EUnrealAIProviderAccessErrorCode::InvalidRequest);
			return false;
		}
		if (Context.IsCancellationRequested())
		{
			OutError =
				MakeHttpError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
			return false;
		}
		if (Context.IsTimedOut())
		{
			OutError =
				MakeHttpError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
			return false;
		}

		@autoreleasepool
		{
			NSString *UrlString = MakeNSString(Request.ExactUrl);
			NSURL *Url = UrlString != nil ? [NSURL URLWithString:UrlString] : nil;
			FString RoundTrip;
			if (Url == nil ||
				!CopyBoundedNSString(Url.absoluteString, FUnrealAIOAuthTrustedAuthorizationServer::MaxUriUtf8Bytes,
									 RoundTrip) ||
				RoundTrip != Request.ExactUrl)
			{
				OutError = MakeHttpError(EUnrealAIErrorCategory::InvalidArgument,
										 EUnrealAIProviderAccessErrorCode::InvalidRequest);
				return false;
			}

			NSMutableURLRequest *NativeRequest = [[NSMutableURLRequest alloc] initWithURL:Url];
			NativeRequest.HTTPMethod = Request.Method == EUnrealAIOAuthIssuerHttpMethod::Get ? @"GET" : @"POST";
			NativeRequest.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
			NativeRequest.timeoutInterval = FMath::Max(0.001, Context.RemainingSeconds());
			[NativeRequest setValue:@"application/json" forHTTPHeaderField:@"Accept"];
			if (Request.Method == EUnrealAIOAuthIssuerHttpMethod::PostForm)
			{
				[NativeRequest setValue:@"application/x-www-form-urlencoded" forHTTPHeaderField:@"Content-Type"];
				NativeRequest.HTTPBody = [NSData dataWithBytes:Request.FormBody.GetData()
														length:static_cast<NSUInteger>(Request.FormBody.Num())];
			}

			NSURLSessionConfiguration *Configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
			Configuration.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
			Configuration.HTTPShouldSetCookies = NO;
			Configuration.URLCredentialStorage = nil;
			Configuration.URLCache = nil;
			Configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
			Configuration.timeoutIntervalForRequest = NativeRequest.timeoutInterval;
			Configuration.timeoutIntervalForResource = NativeRequest.timeoutInterval;
			if (@available(macOS 10.15, *))
			{
				Configuration.TLSMinimumSupportedProtocolVersion = tls_protocol_version_TLSv12;
			}

			TSharedRef<FMacIssuerRequestState, ESPMode::ThreadSafe> State =
				MakeShared<FMacIssuerRequestState, ESPMode::ThreadSafe>(Request.ExactUrl, Request.MaxResponseBodyBytes);
			FAgentMacOAuthIssuerDelegate *Delegate = [[FAgentMacOAuthIssuerDelegate alloc] init];
			Delegate->State = State;
			NSOperationQueue *Queue = [[NSOperationQueue alloc] init];
			Queue.maxConcurrentOperationCount = 1;
			NSURLSession *Session = [NSURLSession sessionWithConfiguration:Configuration
																  delegate:Delegate
															 delegateQueue:Queue];
			NSURLSessionDataTask *Task = [Session dataTaskWithRequest:NativeRequest];
			if (Task == nil)
			{
				[Session invalidateAndCancel];
				while (!State->IsInvalidated())
				{
					State->GetWakeEvent()->Wait(10);
				}
				Delegate->State.Reset();
				OutError = MakeHttpError(EUnrealAIErrorCategory::Transport,
										 EUnrealAIProviderAccessErrorCode::AuthFailed, true);
				return false;
			}
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
			if (Options.AfterNativeTaskCreatedReservedForTesting.IsValid())
			{
				(*Options.AfterNativeTaskCreatedReservedForTesting)();
			}
			if (!Options.bSuspendNativeTaskForTesting)
#endif
			{
				[Task resume];
			}

			bool bCancelled = false;
			bool bTimedOut = false;
			bool bInvalidationStarted = false;
			const uint32 PollMilliseconds =
				static_cast<uint32>(FMath::Max(1, FMath::CeilToInt(Options.CancellationPollSeconds * 1000.0)));
			while (!State->IsInvalidated())
			{
				if (!bInvalidationStarted && Context.IsCancellationRequested())
				{
					bCancelled = true;
					bInvalidationStarted = true;
					[Task cancel];
					[Session invalidateAndCancel];
				}
				else if (!bInvalidationStarted && Context.IsTimedOut())
				{
					bTimedOut = true;
					bInvalidationStarted = true;
					[Task cancel];
					[Session invalidateAndCancel];
				}
				State->GetWakeEvent()->Wait(PollMilliseconds);
			}
			Delegate->State.Reset();
			if (bCancelled)
			{
				OutError =
					MakeHttpError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::AuthCancelled);
				return false;
			}
			if (bTimedOut)
			{
				OutError = MakeHttpError(EUnrealAIErrorCategory::Timeout,
										 EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
				return false;
			}
			if (!State->TakeResponse(OutResponse))
			{
				OutError = MakeHttpError(EUnrealAIErrorCategory::Transport,
										 EUnrealAIProviderAccessErrorCode::AuthFailed, true);
				return false;
			}
			return true;
		}
	}

  private:
	FUnrealAIOAuthIssuerHttpClientOptions Options;
};
} // namespace

TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe>
CreateAgentMacOAuthIssuerHttpClient(const FUnrealAIOAuthIssuerHttpClientOptions &Options)
{
	return MakeShared<FMacOAuthIssuerHttpClient, ESPMode::ThreadSafe>(Options);
}
#endif // PLATFORM_MAC
