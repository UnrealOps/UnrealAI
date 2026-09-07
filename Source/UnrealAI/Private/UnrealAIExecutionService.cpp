// Copyright EngineWorks. All Rights Reserved.

#include "UnrealAIExecutionService.h"

#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include <atomic>
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ScopeLock.h"
#include "UnrealAIProviderAdapter.h"
#include "UnrealAIResponseAdapter.h"
#include "UnrealAIResponseJson.h"
#include "UnrealAIRetryPolicy.h"
#include "UnrealAIStreamChunkQueue.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tests/UnrealAIClientTestSupport.h"
#endif

namespace UnrealAIClientPrivate
{
constexpr int32 MaxErrorBodyBytes = 1024 * 1024;

enum class ERequestMode : uint8
{
	OneShot,
	Stream
};

FString Utf8BytesToString(const TArray<uint8> &Bytes)
{
	if (Bytes.IsEmpty())
	{
		return FString();
	}

	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR *>(Bytes.GetData()), Bytes.Num());
	return FString(Converted.Length(), Converted.Get());
}

FUnrealAIError MakeRequestError(const FString &Message, const FString &Type, const FString &Code, int32 HttpStatus = 0)
{
	FUnrealAIError Error = UnrealAIProviderAdapters::MakeError(Message, HttpStatus);
	Error.Type = Type;
	Error.Code = Code;
	return Error;
}

FUnrealAIError MakeStreamError(const FString &Message, const FString &Code, int32 HttpStatus = 0)
{
	return MakeRequestError(Message, TEXT("stream_error"), Code, HttpStatus);
}

FUnrealAIError MakeCancellationError()
{
	return MakeRequestError(TEXT("The UnrealAI request was cancelled."), TEXT("request_cancelled"),
																			  TEXT("request_cancelled"));
}

EUnrealAIRetryReason GetTransportRetryReason(const FHttpRequestPtr &HttpRequest)
{
	if (HttpRequest.IsValid())
	{
		switch (HttpRequest->GetFailureReason())
		{
		case EHttpFailureReason::ConnectionError:
			return EUnrealAIRetryReason::ConnectionError;
		case EHttpFailureReason::TimedOut:
			return EUnrealAIRetryReason::Timeout;
		default:
			break;
		}
	}
	return EUnrealAIRetryReason::HttpError;
}
} // namespace UnrealAIClientPrivate

namespace UnrealAIClientPrivate
{
class FRequestLifetime final : public IUnrealAIRequestLifetime
{
  public:
	std::atomic<bool> bLogicalComplete{false};
	std::atomic<int32> PhysicalAttempts{0};
	bool IsLogicallyComplete() const override
	{
		return bLogicalComplete.load();
	}
	bool IsPhysicallySettled() const override
	{
		return bLogicalComplete.load() && PhysicalAttempts.load() == 0;
	}
};

struct FPhysicalAttempt
{
	explicit FPhysicalAttempt(TSharedRef<FRequestLifetime, ESPMode::ThreadSafe> InLifetime) : Lifetime(InLifetime)
	{
		++Lifetime->PhysicalAttempts;
	}
	void Complete()
	{
		if (!bComplete.exchange(true))
		{
			--Lifetime->PhysicalAttempts;
		}
	}
	TSharedRef<FRequestLifetime, ESPMode::ThreadSafe> Lifetime;
	std::atomic<bool> bComplete{false};
};
} // namespace UnrealAIClientPrivate

struct FUnrealAIRequestState
{
	TSharedRef<UnrealAIClientPrivate::FRequestLifetime, ESPMode::ThreadSafe> Lifetime =
		MakeShared<UnrealAIClientPrivate::FRequestLifetime, ESPMode::ThreadSafe>();
	TSharedPtr<UnrealAIClientPrivate::FPhysicalAttempt, ESPMode::ThreadSafe> PhysicalAttempt;
	FUnrealAIExecutionLimits Limits;
	double DeadlineSeconds = 0.0;
	int64 ReceivedBytes = 0;
	int32 StreamEvents = 0;
	TArray<uint8> ResponseBodyBytes;
	FCriticalSection QueueMutex;
	FUnrealAIStreamChunkQueue PendingChunks;
	TArray<uint8> ErrorBodyBytes;
	bool bDrainScheduled = false;
	bool bAcceptData = false;
	bool bQueueOverflowed = false;
	int32 HttpStatus = 0;
	int32 AttemptNumber = 0;

	FGuid RequestId;
	FHttpRequestPtr HttpRequest;
	FTSTicker::FDelegateHandle RetryTickerHandle;
	UnrealAIClientPrivate::ERequestMode Mode = UnrealAIClientPrivate::ERequestMode::OneShot;
	FUnrealAIHttpRequestData RequestData;
	EUnrealAIProviderApi ProviderApi = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
	float TimeoutSeconds = 120.0f;
	FUnrealAIRetryPolicy RetryPolicy;
	int32 RetriesAttempted = 0;
	bool bWaitingForRetry = false;
	bool bSawStreamEvent = false;
	bool bTerminal = false;
	int32 ExpectedChoiceCount = 1;

	FUnrealAISseParser SseParser;
	FUnrealAIProviderStreamState ProviderState;
	FUnrealAIChatCompletionNativeDelegate CompletionDelegate;
	FUnrealAIChatStreamEventNativeDelegate EventDelegate;
	FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate;
	FUnrealAIRetryNativeDelegate RetryDelegate;
	TUniquePtr<FUnrealAIResponseState> ResponseState;
	FUnrealAIResponseNativeDelegate ResponseDelegate;
	FUnrealAIResponseEventNativeDelegate ResponseEventDelegate;
#if WITH_DEV_AUTOMATION_TESTS
	TFunction<void()> StartAttemptForTesting;
#endif
};

namespace UnrealAIClientPrivate
{
void DetachHttpRequest(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State)
{
	if (!State.IsValid() || !State->HttpRequest.IsValid())
	{
		return;
	}

	State->HttpRequest->OnStatusCodeReceived().Unbind();
	State->HttpRequest.Reset();
}

void RemoveRetryTicker(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State)
{
	if (State.IsValid() && State->RetryTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(State->RetryTickerHandle);
		State->RetryTickerHandle.Reset();
	}
}

void ResetStreamAttempt(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State, bool bAcceptData)
{
	{
		const FScopeLock Lock(&State->QueueMutex);
		State->PendingChunks.Reset();
		State->bDrainScheduled = false;
		State->bAcceptData = bAcceptData;
		State->bQueueOverflowed = false;
		State->HttpStatus = 0;
		State->ReceivedBytes = 0;
		State->ResponseBodyBytes.Reset();
		State->StreamEvents = 0;
	}
	State->ErrorBodyBytes.Reset();
	State->SseParser = FUnrealAISseParser();
	State->ProviderState = FUnrealAIProviderStreamState();
	State->ProviderState.Response.Model = State->RequestData.ResolvedModel;
	State->ProviderState.ExpectedChoiceCount = State->ExpectedChoiceCount;
	if (State->ResponseState)
	{
		FUnrealAIResponseContext Context = MoveTemp(State->ResponseState->Context);
		*State->ResponseState = FUnrealAIResponseState();
		State->ResponseState->Context = MoveTemp(Context);
	}
}

void ConfigureHttpRequest(const FHttpRequestRef &HttpRequest, const FUnrealAIRequestState &State)
{
	HttpRequest->SetURL(State.RequestData.Url);
	HttpRequest->SetVerb(State.RequestData.Verb);
	if (State.Mode == ERequestMode::Stream)
	{
		HttpRequest->SetTimeout(
			FMath::Max(0.001f, static_cast<float>(State.DeadlineSeconds - FPlatformTime::Seconds())));
		HttpRequest->SetActivityTimeout(FMath::Max(1.0f, State.TimeoutSeconds));
	}
	else
	{
		HttpRequest->SetTimeout(
			FMath::Max(0.001f, static_cast<float>(State.DeadlineSeconds - FPlatformTime::Seconds())));
	}

	for (const TPair<FString, FString> &Header : State.RequestData.Headers)
	{
		HttpRequest->SetHeader(Header.Key, Header.Value);
	}
	HttpRequest->SetContentAsString(State.RequestData.Body);
}
} // namespace UnrealAIClientPrivate

void FUnrealAIExecutionService::Configure(const FUnrealAIProviderConfig &InProviderConfig)
{
	check(IsInGameThread());
	ProviderConfig = InProviderConfig;
	RedactedProviderConfig = InProviderConfig;
	RedactedProviderConfig.ApiKeyOverride.Reset();
	for (TPair<FString, FString> &Header : RedactedProviderConfig.AdditionalHeaders)
	{
		if (Header.Key.Contains(
				TEXT("authorization"), ESearchCase::IgnoreCase) ||
				Header.Key.Contains(TEXT("key"), ESearchCase::IgnoreCase) ||
									Header.Key.Contains(TEXT("token"), ESearchCase::IgnoreCase) ||
														Header.Key.Contains(TEXT("cookie"), ESearchCase::IgnoreCase))
		{
			Header.Value = TEXT("[redacted]");
		}
	}
	bConfigured = true;
}

bool FUnrealAIExecutionService::IsConfigured() const
{
	return bConfigured;
}

const FUnrealAIProviderConfig &FUnrealAIExecutionService::GetProviderConfig() const
{
	return RedactedProviderConfig;
}

FUnrealAIResponseCapabilities FUnrealAIExecutionService::GetResponseCapabilities() const
{
	return UnrealAIResponseAdapters::Capabilities(ProviderConfig);
}

bool FUnrealAIExecutionService::ValidateResponseRequest(const FUnrealAIResponseRequest &Request,
														FUnrealAIError &OutError) const
{
	if (!bConfigured)
	{
		OutError = UnrealAIProviderAdapters::MakeError(TEXT("Client is not configured."));
		return false;
	}
	FUnrealAIHttpRequestData Http;
	FUnrealAIResponseContext Context;
	return UnrealAIResponseAdapters::BuildRequest(ProviderConfig, Request, FString(), EUnrealAIRequestMode::OneShot,
												  Http, Context, OutError);
}

FUnrealAIRequestHandle FUnrealAIExecutionService::CreateResponse(const FUnrealAIResponseRequest &Request,
																 FUnrealAIResponseNativeDelegate CompletionDelegate,
																 FUnrealAIRetryNativeDelegate RetryDelegate)
{
	return StartResponse(Request, false, FUnrealAIResponseEventNativeDelegate(), MoveTemp(CompletionDelegate),
						 MoveTemp(RetryDelegate));
}

FUnrealAIRequestHandle FUnrealAIExecutionService::StreamResponse(const FUnrealAIResponseRequest &Request,
																 FUnrealAIResponseEventNativeDelegate EventDelegate,
																 FUnrealAIResponseNativeDelegate TerminalDelegate,
																 FUnrealAIRetryNativeDelegate RetryDelegate)
{
	return StartResponse(Request, true, MoveTemp(EventDelegate), MoveTemp(TerminalDelegate), MoveTemp(RetryDelegate));
}

FUnrealAIRequestHandle FUnrealAIExecutionService::StartResponse(const FUnrealAIResponseRequest &Request, bool bStream,
																FUnrealAIResponseEventNativeDelegate EventDelegate,
																FUnrealAIResponseNativeDelegate TerminalDelegate,
																FUnrealAIRetryNativeDelegate RetryDelegate)
{
	check(IsInGameThread());
	FUnrealAIResponseResult Failure;
	const FString ApiKey = ResolveApiKey();
	FUnrealAIHttpRequestData Http;
	FUnrealAIResponseContext Context;
	if (!ValidateAdmission(Failure.Error))
	{
	}
	else if (!bConfigured)
	{
		Failure.Error = UnrealAIProviderAdapters::MakeError(TEXT("Client is not configured."));
	}
	else if (ProviderConfig.bRequiresApiKey && ApiKey.IsEmpty())
	{
		Failure.Error = UnrealAIProviderAdapters::MakeError(TEXT("The provider API key is not configured."));
	}
	else
	{
		UnrealAIResponseAdapters::BuildRequest(ProviderConfig, Request, ApiKey,
											   bStream ? EUnrealAIRequestMode::Stream : EUnrealAIRequestMode::OneShot,
											   Http, Context, Failure.Error);
	}
	if (Failure.Error.bIsError)
	{
		TerminalDelegate.ExecuteIfBound(Failure);
		return FUnrealAIRequestHandle();
	}
	const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> State =
		MakeShared<FUnrealAIRequestState, ESPMode::ThreadSafe>();
	State->RequestId = FGuid::NewGuid();
	State->Mode = bStream ? UnrealAIClientPrivate::ERequestMode::Stream : UnrealAIClientPrivate::ERequestMode::OneShot;
	State->RequestData = MoveTemp(Http);
	State->ProviderApi = ProviderConfig.Api;
	State->TimeoutSeconds = ProviderConfig.TimeoutSeconds;
	State->Limits = Limits;
	State->DeadlineSeconds = FPlatformTime::Seconds() + ProviderConfig.TimeoutSeconds;
	State->RetryPolicy = UnrealAIRetryPolicy::Resolve(ProviderConfig.RetryPolicy, Request.RetryOptions);
	State->RetryDelegate = MoveTemp(RetryDelegate);
	State->ResponseDelegate = MoveTemp(TerminalDelegate);
	State->ResponseEventDelegate = MoveTemp(EventDelegate);
	State->ResponseState = MakeUnique<FUnrealAIResponseState>();
	State->ResponseState->Context = MoveTemp(Context);
	ActiveRequests.Add(State->RequestId, State);
	OutstandingLifetimes.Add(State->Lifetime);
	EnsureDeadlineTicker();
	StartRequestAttempt(State);
	FUnrealAIRequestHandle Handle;
	if (ActiveRequests.Contains(State->RequestId))
	{
		Handle.Id = State->RequestId;
		Handle.Lifetime = State->Lifetime;
	}
	return Handle;
}

FUnrealAIRequestHandle
FUnrealAIExecutionService::CreateChatCompletion(const FUnrealAIChatRequest &Request,
												FUnrealAIChatCompletionNativeDelegate CompletionDelegate,
												FUnrealAIRetryNativeDelegate RetryDelegate)
{
	check(IsInGameThread());
	auto FailBeforeStart = [&CompletionDelegate](const FUnrealAIError &Error)
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(EmptyResponse, Error);
	};

	FUnrealAIError AdmissionError;
	if (!ValidateAdmission(AdmissionError))
	{
		FailBeforeStart(AdmissionError);
		return {};
	}

	if (!bConfigured)
	{
		FailBeforeStart(UnrealAIProviderAdapters::MakeError(TEXT("Client is not configured.")));
		return FUnrealAIRequestHandle();
	}

	if (Request.bStream)
	{
		FUnrealAIError Error = UnrealAIProviderAdapters::MakeError(
			TEXT("The bStream request field is deprecated. Use StreamChatCompletion or the Stream Chat Completion Blueprint node."));
		Error.Type = TEXT("invalid_request_error");
		Error.Code = TEXT("deprecated_stream_flag");
		FailBeforeStart(Error);
		return FUnrealAIRequestHandle();
	}

	const FString ApiKey = ResolveApiKey();
	if (ProviderConfig.bRequiresApiKey && ApiKey.IsEmpty())
	{
		FailBeforeStart(UnrealAIProviderAdapters::MakeError(
			FString::Printf(TEXT("API key is not configured. Set %s or provide an API key override."),
								 *ProviderConfig.ApiKeyEnvironmentVariable)));
		return FUnrealAIRequestHandle();
	}

	const IUnrealAIProviderAdapter &Adapter = UnrealAIProviderAdapters::Get(ProviderConfig.Api);
	FUnrealAIHttpRequestData RequestData;
	FUnrealAIError BuildError;
	if (!Adapter.BuildRequest(ProviderConfig, Request, ApiKey, EUnrealAIRequestMode::OneShot, RequestData, BuildError))
	{
		FailBeforeStart(BuildError);
		return FUnrealAIRequestHandle();
	}

	const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> State =
		MakeShared<FUnrealAIRequestState, ESPMode::ThreadSafe>();
	State->RequestId = FGuid::NewGuid();
	State->Mode = UnrealAIClientPrivate::ERequestMode::OneShot;
	State->RequestData = MoveTemp(RequestData);
	State->ProviderApi = ProviderConfig.Api;
	State->TimeoutSeconds = ProviderConfig.TimeoutSeconds;
	State->Limits = Limits;
	State->DeadlineSeconds = FPlatformTime::Seconds() + ProviderConfig.TimeoutSeconds;
	State->RetryPolicy = UnrealAIRetryPolicy::Resolve(ProviderConfig.RetryPolicy, Request.RetryOptions);
	State->CompletionDelegate = MoveTemp(CompletionDelegate);
	State->RetryDelegate = MoveTemp(RetryDelegate);

	ActiveRequests.Add(State->RequestId, State);
	OutstandingLifetimes.Add(State->Lifetime);
	EnsureDeadlineTicker();
	StartRequestAttempt(State);

	FUnrealAIRequestHandle Handle;
	if (ActiveRequests.Contains(State->RequestId))
	{
		Handle.Id = State->RequestId;
		Handle.Lifetime = State->Lifetime;
	}
	return Handle;
}

FUnrealAIRequestHandle FUnrealAIExecutionService::StreamChatCompletion(
	const FUnrealAIChatRequest &Request, FUnrealAIChatStreamEventNativeDelegate EventDelegate,
	FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate, FUnrealAIRetryNativeDelegate RetryDelegate)
{
	check(IsInGameThread());
	auto FailBeforeStart = [&TerminalDelegate](const FUnrealAIError &Error)
	{
		FUnrealAIChatStreamResult Result;
		Result.Status = EUnrealAIChatStreamStatus::Failed;
		Result.Error = Error;
		TerminalDelegate.ExecuteIfBound(Result);
	};

	FUnrealAIError AdmissionError;
	if (!ValidateAdmission(AdmissionError))
	{
		FailBeforeStart(AdmissionError);
		return {};
	}

	if (!bConfigured)
	{
		FailBeforeStart(UnrealAIProviderAdapters::MakeError(TEXT("Client is not configured.")));
		return FUnrealAIRequestHandle();
	}

	const FString ApiKey = ResolveApiKey();
	if (ProviderConfig.bRequiresApiKey && ApiKey.IsEmpty())
	{
		FailBeforeStart(UnrealAIProviderAdapters::MakeError(
			FString::Printf(TEXT("API key is not configured. Set %s or provide an API key override."),
								 *ProviderConfig.ApiKeyEnvironmentVariable)));
		return FUnrealAIRequestHandle();
	}

	const IUnrealAIProviderAdapter &Adapter = UnrealAIProviderAdapters::Get(ProviderConfig.Api);
	FUnrealAIHttpRequestData RequestData;
	FUnrealAIError BuildError;
	if (!Adapter.BuildRequest(ProviderConfig, Request, ApiKey, EUnrealAIRequestMode::Stream, RequestData, BuildError))
	{
		FailBeforeStart(BuildError);
		return FUnrealAIRequestHandle();
	}

	const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> State =
		MakeShared<FUnrealAIRequestState, ESPMode::ThreadSafe>();
	State->RequestId = FGuid::NewGuid();
	State->Mode = UnrealAIClientPrivate::ERequestMode::Stream;
	State->RequestData = MoveTemp(RequestData);
	State->ProviderApi = ProviderConfig.Api;
	State->TimeoutSeconds = ProviderConfig.TimeoutSeconds;
	State->Limits = Limits;
	State->DeadlineSeconds = FPlatformTime::Seconds() + ProviderConfig.TimeoutSeconds;
	State->RetryPolicy = UnrealAIRetryPolicy::Resolve(ProviderConfig.RetryPolicy, Request.RetryOptions);
	State->ExpectedChoiceCount = FMath::Max(1, Request.NumChoices);
	State->EventDelegate = MoveTemp(EventDelegate);
	State->TerminalDelegate = MoveTemp(TerminalDelegate);
	State->RetryDelegate = MoveTemp(RetryDelegate);

	ActiveRequests.Add(State->RequestId, State);
	OutstandingLifetimes.Add(State->Lifetime);
	EnsureDeadlineTicker();
	StartRequestAttempt(State);

	FUnrealAIRequestHandle Handle;
	if (ActiveRequests.Contains(State->RequestId))
	{
		Handle.Id = State->RequestId;
		Handle.Lifetime = State->Lifetime;
	}
	return Handle;
}

void FUnrealAIExecutionService::StartRequestAttempt(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State)
{
	if (!State.IsValid() || State->bTerminal || !ActiveRequests.Contains(State->RequestId))
	{
		return;
	}

	const FTCHARToUTF8 RequestUtf8(*State->RequestData.Body);
	if (RequestUtf8.Length() > State->Limits.MaxRequestBytes)
	{
		const FUnrealAIError Error =
			UnrealAIClientPrivate::MakeRequestError(TEXT("The serialized model request exceeds its byte limit."),
														 TEXT("invalid_request_error"), TEXT("request_too_large"));
		if (State->Mode == UnrealAIClientPrivate::ERequestMode::Stream)
		{
			CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, Error);
		}
		else
		{
			CompleteOneShotRequest(State, {}, Error);
		}
		return;
	}

	State->RetryTickerHandle.Reset();
	State->bWaitingForRetry = false;
	++State->AttemptNumber;
	const int32 AttemptNumber = State->AttemptNumber;

	if (State->Mode == UnrealAIClientPrivate::ERequestMode::Stream)
	{
		UnrealAIClientPrivate::ResetStreamAttempt(State, true);
	}

#if WITH_DEV_AUTOMATION_TESTS
	if (State->StartAttemptForTesting)
	{
		State->StartAttemptForTesting();
		return;
	}
#endif

	FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
	State->HttpRequest = HttpRequest;
	UnrealAIClientPrivate::ConfigureHttpRequest(HttpRequest, *State);
	const TSharedRef<UnrealAIClientPrivate::FPhysicalAttempt, ESPMode::ThreadSafe> Physical =
		MakeShared<UnrealAIClientPrivate::FPhysicalAttempt, ESPMode::ThreadSafe>(State->Lifetime);
	State->PhysicalAttempt = Physical;
	const TWeakPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> WeakService = AsShared();
	const FGuid LogicalId = State->RequestId;

	if (State->Mode == UnrealAIClientPrivate::ERequestMode::OneShot)
	{
		HttpRequest->OnProcessRequestComplete().BindLambda(
			[WeakService, Physical, LogicalId, AttemptNumber](FHttpRequestPtr Request, FHttpResponsePtr Response,
															  bool bSuccess)
			{
				ON_SCOPE_EXIT
				{
					Physical->Complete();
				};
				if (TSharedPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> Service = WeakService.Pin())
				{
					Service->HandleChatCompletionResponse(Request, Response, bSuccess, LogicalId, AttemptNumber);
				}
			});
		State->ResponseBodyBytes.Reset();
		State->ReceivedBytes = 0;
		State->bQueueOverflowed = false;
		TWeakPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> WeakState = State;
		FHttpRequestStreamDelegateV2 Receive;
		Receive.BindLambda(
			[WeakState, AttemptNumber](void *Data, int64 &Length)
			{
				const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> Pinned = WeakState.Pin();
				if (!Pinned)
				{
					Length = 0;
					return;
				}
				FScopeLock Lock(&Pinned->QueueMutex);
				if (!Data || Length <= 0 || Pinned->Lifetime->bLogicalComplete.load() ||
					Pinned->AttemptNumber != AttemptNumber)
				{
					Length = 0;
					return;
				}
				if (Length > Pinned->Limits.MaxResponseBytes - Pinned->ReceivedBytes)
				{
					Pinned->bQueueOverflowed = true;
					Length = 0;
					return;
				}
				Pinned->ReceivedBytes += Length;
				Pinned->ResponseBodyBytes.Append(static_cast<const uint8 *>(Data), static_cast<int32>(Length));
			});
		if (!HttpRequest->SetResponseBodyReceiveStreamDelegateV2(Receive))
		{
			Physical->Complete();
			CompleteOneShotRequest(State, {},
								   UnrealAIClientPrivate::MakeRequestError(
									   TEXT("The HTTP backend does not support bounded response reception."),
											TEXT("transport_error"), TEXT("bounded_transport_unavailable")));
			return;
		}
	}
	else
	{
		TWeakPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> WeakThis = AsShared();
		TWeakPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> WeakState = State;
		FHttpRequestStreamDelegateV2 StreamDelegate;
		StreamDelegate.BindLambda(
			[WeakState, WeakThis, AttemptNumber](void *Data, int64 &Length)
			{
				if (!Data || Length <= 0 || Length > MAX_int32)
				{
					Length = 0;
					return;
				}

				const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> PinnedState = WeakState.Pin();
				if (!PinnedState.IsValid())
				{
					Length = 0;
					return;
				}

				bool bScheduleDrain = false;
				{
					const FScopeLock Lock(&PinnedState->QueueMutex);
					if (!PinnedState->bAcceptData || PinnedState->AttemptNumber != AttemptNumber)
					{
						Length = 0;
						return;
					}

					if (Length > PinnedState->Limits.MaxResponseBytes - PinnedState->ReceivedBytes ||
						!PinnedState->PendingChunks.Enqueue(static_cast<const uint8 *>(Data), Length))
					{
						PinnedState->bAcceptData = false;
						PinnedState->bQueueOverflowed = true;
						Length = 0;
					}
					PinnedState->ReceivedBytes += Length;
					if (!PinnedState->bDrainScheduled)
					{
						PinnedState->bDrainScheduled = true;
						bScheduleDrain = true;
					}
				}

				if (bScheduleDrain)
				{
					AsyncTask(ENamedThreads::GameThread,
							  [WeakThis, RequestId = PinnedState->RequestId]()
							  {
								  if (TSharedPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> Client =
										  WeakThis.Pin())
								  {
									  Client->DrainStreamRequest(RequestId);
								  }
							  });
				}
			});
		if (!HttpRequest->SetResponseBodyReceiveStreamDelegateV2(StreamDelegate))
		{
			Physical->Complete();
			UnrealAIClientPrivate::DetachHttpRequest(State);
			CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed,
								  UnrealAIClientPrivate::MakeStreamError(
									  TEXT("The active Unreal HTTP backend does not support response streaming."),
										   TEXT("stream_transport_unavailable")));
			return;
		}

		HttpRequest->OnStatusCodeReceived().BindLambda(
			[WeakState, WeakThis, AttemptNumber](FHttpRequestPtr RequestPtr, int32 StatusCode)
			{
				(void)RequestPtr;
				const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> PinnedState = WeakState.Pin();
				if (!PinnedState.IsValid())
				{
					return;
				}

				bool bScheduleDrain = false;
				{
					const FScopeLock Lock(&PinnedState->QueueMutex);
					if (PinnedState->AttemptNumber != AttemptNumber)
					{
						return;
					}
					if (StatusCode > 0)
					{
						PinnedState->HttpStatus = StatusCode;
					}
					if (PinnedState->HttpStatus != 0 && !PinnedState->PendingChunks.IsEmpty() &&
						!PinnedState->bDrainScheduled)
					{
						PinnedState->bDrainScheduled = true;
						bScheduleDrain = true;
					}
				}

				if (bScheduleDrain)
				{
					AsyncTask(ENamedThreads::GameThread,
							  [WeakThis, RequestId = PinnedState->RequestId]()
							  {
								  if (TSharedPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> Client =
										  WeakThis.Pin())
								  {
									  Client->DrainStreamRequest(RequestId);
								  }
							  });
				}
			});
		HttpRequest->OnProcessRequestComplete().BindLambda(
			[WeakService, Physical, LogicalId, AttemptNumber](FHttpRequestPtr Request, FHttpResponsePtr Response,
															  bool bSuccess)
			{
				ON_SCOPE_EXIT
				{
					Physical->Complete();
				};
				if (TSharedPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> Service = WeakService.Pin())
				{
					Service->HandleStreamResponse(Request, Response, bSuccess, LogicalId, AttemptNumber);
				}
			});
	}

	if (!HttpRequest->ProcessRequest())
	{
		Physical->Complete();
		UnrealAIClientPrivate::DetachHttpRequest(State);
		if (State->Mode == UnrealAIClientPrivate::ERequestMode::Stream)
		{
			CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed,
								  UnrealAIClientPrivate::MakeStreamError(
									  TEXT("Failed to start streaming HTTP request."), TEXT("stream_start_failed")));
		}
		else
		{
			FUnrealAIChatResponse EmptyResponse;
			CompleteOneShotRequest(
				State, EmptyResponse,
				UnrealAIClientPrivate::MakeRequestError(TEXT("Failed to start HTTP request."),
															 TEXT("transport_error"), TEXT("request_start_failed")));
		}
	}
}

void FUnrealAIExecutionService::ResumeRequestAfterBackoff(const FGuid &RequestId)
{
	if (TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> *StatePtr = ActiveRequests.Find(RequestId))
	{
		StartRequestAttempt(*StatePtr);
	}
}

bool FUnrealAIExecutionService::CancelRequest(const FUnrealAIRequestHandle &RequestHandle)
{
	check(IsInGameThread());
	if (!RequestHandle.IsValid())
	{
		return false;
	}

	TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> *StatePtr = ActiveRequests.Find(RequestHandle.Id);
	if (!StatePtr)
	{
		return false;
	}

	if ((*StatePtr)->Mode == UnrealAIClientPrivate::ERequestMode::Stream && !(*StatePtr)->bWaitingForRetry)
	{
		DrainStreamRequest(RequestHandle.Id);
		StatePtr = ActiveRequests.Find(RequestHandle.Id);
		if (!StatePtr)
		{
			return false;
		}
	}

	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> State = *StatePtr;
	const FHttpRequestPtr HttpRequest = State->HttpRequest;
	if (State->Mode == UnrealAIClientPrivate::ERequestMode::Stream)
	{
		FUnrealAIError NoError;
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Cancelled, NoError);
	}
	else
	{
		FUnrealAIChatResponse EmptyResponse;
		CompleteOneShotRequest(State, EmptyResponse, UnrealAIClientPrivate::MakeCancellationError());
	}

	if (HttpRequest.IsValid())
	{
		HttpRequest->CancelRequest();
	}
	return true;
}

FString FUnrealAIExecutionService::ResolveApiKey() const
{
	if (!ProviderConfig.ApiKeyOverride.IsEmpty())
	{
		return ProviderConfig.ApiKeyOverride;
	}

	if (!ProviderConfig.ApiKeyEnvironmentVariable.IsEmpty())
	{
		return FPlatformMisc::GetEnvironmentVariable(*ProviderConfig.ApiKeyEnvironmentVariable);
	}

	return FString();
}

void FUnrealAIExecutionService::HandleChatCompletionResponse(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse,
															 bool bWasSuccessful, FGuid RequestId, int32 AttemptNumber)
{
	TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> *StatePtr = ActiveRequests.Find(RequestId);
	if (!StatePtr || (*StatePtr)->bTerminal || (*StatePtr)->AttemptNumber != AttemptNumber)
	{
		return;
	}

	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> State = *StatePtr;
	UnrealAIClientPrivate::DetachHttpRequest(State);

	if (State->bQueueOverflowed)
	{
		CompleteOneShotRequest(
			State, {},
			UnrealAIClientPrivate::MakeRequestError(TEXT("The model response exceeded its byte limit."),
														 TEXT("response_error"), TEXT("response_too_large")));
		return;
	}
	FUnrealAIChatResponse ParsedResponse;
	FUnrealAIError Error;
	if (!bWasSuccessful || !HttpResponse.IsValid())
	{
		Error = UnrealAIClientPrivate::MakeRequestError(
			TEXT("HTTP request failed before a provider response was received."),
				 TEXT("transport_error"), TEXT("request_transport_failed"));
		const EUnrealAIRetryReason Reason = UnrealAIClientPrivate::GetTransportRetryReason(HttpRequest);
		if (TryScheduleRetry(State, Reason, 0, Error, HttpResponse))
		{
			return;
		}
		CompleteOneShotRequest(State, ParsedResponse, Error);
		return;
	}

	if (State->ResponseState)
	{
		UnrealAIResponseAdapters::ParseResponse(HttpResponse->GetResponseCode(),
												UnrealAIClientPrivate::Utf8BytesToString(State->ResponseBodyBytes),
												*State->ResponseState);
		Error = State->ResponseState->Result.Error;
	}
	else
	{
		UnrealAIProviderAdapters::Get(State->ProviderApi)
			.ParseResponse(State->RequestData.ResolvedModel, HttpResponse->GetResponseCode(),
						   UnrealAIClientPrivate::Utf8BytesToString(State->ResponseBodyBytes), ParsedResponse, Error);
	}
	if (Error.bIsError &&
		TryScheduleRetry(State, EUnrealAIRetryReason::HttpError, HttpResponse->GetResponseCode(), Error, HttpResponse))
	{
		return;
	}
	CompleteOneShotRequest(State, ParsedResponse, Error);
}

bool FUnrealAIExecutionService::ProcessSseEvent(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State,
												const FUnrealAISseEvent &Frame, FUnrealAIError &OutError)
{
	if (++State->StreamEvents > State->Limits.MaxStreamEvents)
	{
		OutError = UnrealAIClientPrivate::MakeStreamError(TEXT("The model stream exceeded its event limit."),
															   TEXT("stream_event_limit"));
		return false;
	}
	State->bSawStreamEvent = true;
	if (State->ResponseState)
	{
		TArray<FUnrealAIResponseEvent> Events;
		if (!UnrealAIResponseAdapters::ParseStreamEvent(Frame, *State->ResponseState, Events, OutError))
		{
			return false;
		}
		for (FUnrealAIResponseEvent &Event : Events)
		{
			Event.RequestHandle.Id = State->RequestId;
			Event.RequestHandle.Lifetime = State->Lifetime;
			State->ResponseEventDelegate.ExecuteIfBound(Event);
			if (State->bTerminal)
			{
				break;
			}
		}
	}
	else
	{
		TArray<FUnrealAIChatStreamEvent> Events;
		if (!UnrealAIProviderAdapters::Get(State->ProviderApi)
				 .ParseStreamEvent(Frame, State->ProviderState, Events, OutError))
		{
			return false;
		}
		for (const FUnrealAIChatStreamEvent &Event : Events)
		{
			State->EventDelegate.ExecuteIfBound(Event);
			if (State->bTerminal)
			{
				break;
			}
		}
	}
	return true;
}

void FUnrealAIExecutionService::DrainStreamRequest(const FGuid &RequestId)
{
	TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> *StatePtr = ActiveRequests.Find(RequestId);
	if (!StatePtr || (*StatePtr)->bTerminal || (*StatePtr)->Mode != UnrealAIClientPrivate::ERequestMode::Stream ||
		(*StatePtr)->bWaitingForRetry)
	{
		return;
	}

	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> State = *StatePtr;
	TArray<TArray<uint8>> Chunks;
	int32 HttpStatus = 0;
	bool bQueueOverflowed = false;
	{
		const FScopeLock Lock(&State->QueueMutex);
		HttpStatus = State->HttpStatus;
		bQueueOverflowed = State->bQueueOverflowed;
		if (HttpStatus == 0 && !bQueueOverflowed)
		{
			State->bDrainScheduled = false;
			return;
		}
		State->PendingChunks.Drain(Chunks);
		State->bDrainScheduled = false;
	}
	if (bQueueOverflowed)
	{
		const FHttpRequestPtr HttpRequest = State->HttpRequest;
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed,
							  UnrealAIClientPrivate::MakeStreamError(
								  TEXT("The HTTP stream exceeded the 4 MiB pending-data safety limit."),
									   TEXT("stream_buffer_overflow"), HttpStatus));
		if (HttpRequest.IsValid())
		{
			HttpRequest->CancelRequest();
		}
		return;
	}

	for (const TArray<uint8> &Chunk : Chunks)
	{
		if (HttpStatus >= 300)
		{
			const int32 RemainingBytes = UnrealAIClientPrivate::MaxErrorBodyBytes - State->ErrorBodyBytes.Num();
			if (RemainingBytes > 0)
			{
				State->ErrorBodyBytes.Append(Chunk.GetData(), FMath::Min(RemainingBytes, Chunk.Num()));
			}
			continue;
		}

		TArray<FUnrealAISseEvent> SseEvents;
		FUnrealAIError ParseError;
		if (!State->SseParser.Append(Chunk.GetData(), Chunk.Num(), SseEvents, ParseError))
		{
			const FHttpRequestPtr HttpRequest = State->HttpRequest;
			CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ParseError);
			if (HttpRequest.IsValid())
			{
				HttpRequest->CancelRequest();
			}
			return;
		}

		for (const FUnrealAISseEvent &SseEvent : SseEvents)
		{
			if (!ProcessSseEvent(State, SseEvent, ParseError))
			{
				const FHttpRequestPtr HttpRequest = State->HttpRequest;
				CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ParseError);
				if (HttpRequest.IsValid())
				{
					HttpRequest->CancelRequest();
				}
				return;
			}

			if (!ActiveRequests.Contains(RequestId))
			{
				return;
			}
		}
	}
}

void FUnrealAIExecutionService::HandleStreamResponse(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse,
													 bool bWasSuccessful, FGuid RequestId, int32 AttemptNumber)
{
	TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> *StatePtr = ActiveRequests.Find(RequestId);
	if (!StatePtr || (*StatePtr)->bTerminal || (*StatePtr)->AttemptNumber != AttemptNumber)
	{
		return;
	}

	if (HttpResponse.IsValid())
	{
		const FScopeLock Lock(&(*StatePtr)->QueueMutex);
		(*StatePtr)->HttpStatus = HttpResponse->GetResponseCode();
	}

	DrainStreamRequest(RequestId);
	StatePtr = ActiveRequests.Find(RequestId);
	if (!StatePtr || (*StatePtr)->AttemptNumber != AttemptNumber)
	{
		return;
	}

	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> State = *StatePtr;
	UnrealAIClientPrivate::DetachHttpRequest(State);
	const int32 HttpStatus = HttpResponse.IsValid() ? HttpResponse->GetResponseCode() : State->HttpStatus;
	if (!bWasSuccessful || !HttpResponse.IsValid())
	{
		const FUnrealAIError Error = UnrealAIClientPrivate::MakeStreamError(
			TEXT("Streaming HTTP request ended before a complete provider response was received."),
				 TEXT("stream_transport_failed"), HttpStatus);
		const EUnrealAIRetryReason Reason = UnrealAIClientPrivate::GetTransportRetryReason(HttpRequest);
		if (!State->bSawStreamEvent && TryScheduleRetry(State, Reason, HttpStatus, Error, HttpResponse))
		{
			return;
		}
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, Error);
		return;
	}

	if (HttpStatus < 200 || HttpStatus >= 300)
	{
		FUnrealAIChatResponse IgnoredResponse;
		FUnrealAIError ProviderError;
		if (State->ResponseState)
		{
			UnrealAIResponseAdapters::ParseResponse(
				HttpStatus, UnrealAIClientPrivate::Utf8BytesToString(State->ErrorBodyBytes), *State->ResponseState);
			ProviderError = State->ResponseState->Result.Error;
		}
		else
		{
			UnrealAIProviderAdapters::Get(State->ProviderApi)
				.ParseResponse(State->RequestData.ResolvedModel, HttpStatus,
							   UnrealAIClientPrivate::Utf8BytesToString(State->ErrorBodyBytes), IgnoredResponse,
							   ProviderError);
		}
		if (!State->bSawStreamEvent &&
			TryScheduleRetry(State, EUnrealAIRetryReason::HttpError, HttpStatus, ProviderError, HttpResponse))
		{
			return;
		}
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ProviderError);
		return;
	}

	const FString ContentType = HttpResponse->GetContentType();
	if (!ContentType.Contains(TEXT("text/event-stream"), ESearchCase::IgnoreCase))
	{
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed,
							  UnrealAIClientPrivate::MakeStreamError(FString::Printf(
								  TEXT("Provider returned '%s' instead of text/event-stream."), *ContentType),
								  TEXT("invalid_stream_content_type"), HttpStatus));
		return;
	}

	TArray<FUnrealAISseEvent> FinalSseEvents;
	FUnrealAIError ParseError;
	if (!State->SseParser.Finish(FinalSseEvents, ParseError))
	{
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ParseError);
		return;
	}
	for (const FUnrealAISseEvent &SseEvent : FinalSseEvents)
	{
		if (!ProcessSseEvent(State, SseEvent, ParseError))
		{
			CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ParseError);
			return;
		}
		if (!ActiveRequests.Contains(RequestId))
		{
			return;
		}
	}

	const bool bComplete =
		State->ResponseState
			? UnrealAIResponseAdapters::CanCompleteStream(*State->ResponseState)
			: UnrealAIProviderAdapters::Get(State->ProviderApi).CanCompleteStream(State->ProviderState);
	if (!bComplete)
	{
		const FUnrealAIError Error = UnrealAIClientPrivate::MakeStreamError(TEXT("The provider stream ended without its required terminal event."), TEXT("truncated_stream"), HttpStatus);
		if (!State->bSawStreamEvent &&
			TryScheduleRetry(State, EUnrealAIRetryReason::EmptyStream, HttpStatus, Error, HttpResponse))
		{
			return;
		}
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, Error);
		return;
	}

	FUnrealAIError NoError;
	if (State->ResponseState)
	{
		UnrealAIResponseAdapters::Normalize(*State->ResponseState, true);
		NoError = State->ResponseState->Result.Error;
	}
	CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Completed, NoError);
}

bool FUnrealAIExecutionService::TryScheduleRetry(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State,
												 EUnrealAIRetryReason Reason, int32 HttpStatus,
												 const FUnrealAIError &Error, const FHttpResponsePtr &HttpResponse)
{
	if (!State.IsValid() || State->bTerminal || !ActiveRequests.Contains(State->RequestId) ||
		(State->Mode == UnrealAIClientPrivate::ERequestMode::Stream &&
		 !UnrealAIRetryPolicy::CanRetryStream(State->bSawStreamEvent)))
	{
		return false;
	}

	FUnrealAIRetryFailure Failure;
	Failure.Reason = Reason;
	Failure.HttpStatus = HttpStatus;
	Failure.Error = Error;
	if (HttpResponse.IsValid())
	{
		Failure.RetryAfter = HttpResponse->GetHeader(TEXT("Retry-After"));
	}

	if (!UnrealAIRetryPolicy::IsRetryable(State->RetryPolicy, State->RetriesAttempted, Failure))
	{
		return false;
	}

	const int32 RetryNumber = State->RetriesAttempted + 1;
	float DelaySeconds = 0.0f;
	if (!UnrealAIRetryPolicy::TryComputeDelay(State->RetryPolicy, RetryNumber, Failure.RetryAfter, FDateTime::UtcNow(),
											  FMath::FRand(), DelaySeconds))
	{
		return false;
	}

	if (State->DeadlineSeconds > 0.0 && FPlatformTime::Seconds() + DelaySeconds >= State->DeadlineSeconds)
	{
		return false;
	}

	State->RetriesAttempted = RetryNumber;
	State->bWaitingForRetry = true;
	if (State->Mode == UnrealAIClientPrivate::ERequestMode::Stream)
	{
		UnrealAIClientPrivate::ResetStreamAttempt(State, false);
	}

	FUnrealAIRetryEvent RetryEvent;
	RetryEvent.RequestHandle.Id = State->RequestId;
	RetryEvent.RequestHandle.Lifetime = State->Lifetime;
	RetryEvent.RetryNumber = RetryNumber;
	RetryEvent.MaxRetries = State->RetryPolicy.MaxRetries;
	RetryEvent.DelaySeconds = DelaySeconds;
	RetryEvent.Reason = Reason;
	RetryEvent.HttpStatus = HttpStatus;
	State->RetryDelegate.ExecuteIfBound(RetryEvent);
	if (State->bTerminal || !ActiveRequests.Contains(State->RequestId))
	{
		return true;
	}

	TWeakPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> WeakThis = AsShared();
	State->RetryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("UnrealAI request retry"), DelaySeconds,
			 [WeakThis, RequestId = State->RequestId](float DeltaSeconds)
			 {
				 (void)DeltaSeconds;
				 if (TSharedPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> Client = WeakThis.Pin())
				 {
					 Client->ResumeRequestAfterBackoff(RequestId);
				 }
				 return false;
			 });
	return true;
}

void FUnrealAIExecutionService::DeliverResponseTerminal(
	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State, bool bCancelled, const FUnrealAIError &Error)
{
	if (!State->ResponseState)
	{
		return;
	}
	FUnrealAIResponseResult Result = MoveTemp(State->ResponseState->Result);
	if (State->ResponseState->Snapshot)
	{
		// Preserve the latest opaque data even when interruption precedes item completion.
		Result.Response.RawJson = UnrealAIResponseJson::Serialize(State->ResponseState->Snapshot);
	}
	Result.RequestHandle.Id = State->RequestId;
	Result.RequestHandle.Lifetime = State->Lifetime;
	if (bCancelled)
	{
		Result.Status = EUnrealAIResponseStatus::Cancelled;
		Result.Error = FUnrealAIError();
	}
	else if (Error.bIsError)
	{
		Result.Status = EUnrealAIResponseStatus::Failed;
		Result.Error = Error;
	}
	if (Result.Status != EUnrealAIResponseStatus::Completed)
	{
		Result.Response.Continuation.ItemsJson.Reset();
	}
	State->ResponseDelegate.ExecuteIfBound(Result);
	State->ResponseDelegate.Unbind();
	State->ResponseEventDelegate.Unbind();
	State->ResponseState.Reset();
}

void FUnrealAIExecutionService::CompleteOneShotRequest(
	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State, const FUnrealAIChatResponse &Response,
	const FUnrealAIError &Error)
{
	if (!State.IsValid() || State->bTerminal)
	{
		return;
	}

	State->bTerminal = true;
	State->Lifetime->bLogicalComplete.store(true);
	UnrealAIClientPrivate::RemoveRetryTicker(State);
	UnrealAIClientPrivate::DetachHttpRequest(State);
	ActiveRequests.Remove(State->RequestId);
	State->RequestData.Headers.Reset();
	State->RequestData.Body.Reset();

	DeliverResponseTerminal(State, Error.Code == TEXT("request_cancelled"), Error);
	State->CompletionDelegate.ExecuteIfBound(Response, Error);
	State->CompletionDelegate.Unbind();
	State->RetryDelegate.Unbind();
}

void FUnrealAIExecutionService::CompleteStreamRequest(
	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State, EUnrealAIChatStreamStatus Status,
	const FUnrealAIError &Error)
{
	if (!State.IsValid() || State->bTerminal)
	{
		return;
	}

	State->bTerminal = true;
	State->Lifetime->bLogicalComplete.store(true);
	UnrealAIClientPrivate::RemoveRetryTicker(State);
	{
		const FScopeLock Lock(&State->QueueMutex);
		State->bAcceptData = false;
		State->PendingChunks.Reset();
		State->bDrainScheduled = false;
	}
	UnrealAIClientPrivate::DetachHttpRequest(State);
	ActiveRequests.Remove(State->RequestId);
	State->RequestData.Headers.Reset();
	State->RequestData.Body.Reset();

	State->ProviderState.Response.RawJson.Reset();
	State->ProviderState.Response.Choices.Sort([](const FUnrealAIChatChoice &Left, const FUnrealAIChatChoice &Right)
											   { return Left.Index < Right.Index; });

	FUnrealAIChatStreamResult Result;
	Result.Status = Status;
	Result.Response = State->ProviderState.Response;
	Result.Error = Error;
	DeliverResponseTerminal(State, Status == EUnrealAIChatStreamStatus::Cancelled, Error);
	State->TerminalDelegate.ExecuteIfBound(Result);
	State->EventDelegate.Unbind();
	State->TerminalDelegate.Unbind();
	State->RetryDelegate.Unbind();
}

void FUnrealAIExecutionService::CancelAllRequests()
{
	TArray<TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>> States;
	ActiveRequests.GenerateValueArray(States);
	ActiveRequests.Reset();
	for (const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State : States)
	{
		if (!State.IsValid())
		{
			continue;
		}

		State->bTerminal = true;
		State->Lifetime->bLogicalComplete.store(true);
		UnrealAIClientPrivate::RemoveRetryTicker(State);
		{
			const FScopeLock Lock(&State->QueueMutex);
			State->bAcceptData = false;
			State->PendingChunks.Reset();
		}
		State->CompletionDelegate.Unbind();
		State->EventDelegate.Unbind();
		State->TerminalDelegate.Unbind();
		State->RetryDelegate.Unbind();
		State->ResponseDelegate.Unbind();
		State->ResponseEventDelegate.Unbind();
		State->ResponseState.Reset();
		const FHttpRequestPtr HttpRequest = State->HttpRequest;
		UnrealAIClientPrivate::DetachHttpRequest(State);
		State->RequestData.Headers.Reset();
		State->RequestData.Body.Reset();
		if (HttpRequest.IsValid())
		{
			HttpRequest->CancelRequest();
		}
	}
}

#if WITH_DEV_AUTOMATION_TESTS

void FUnrealAIClientTestAccess::RunRetryCoordinatorTests(FAutomationTestBase &Test)
{
	TSharedRef<FUnrealAIExecutionService, ESPMode::ThreadSafe> Client =
		MakeShared<FUnrealAIExecutionService, ESPMode::ThreadSafe>();

	auto MakeState = [Client](UnrealAIClientPrivate::ERequestMode Mode)
	{
		const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> State =
			MakeShared<FUnrealAIRequestState, ESPMode::ThreadSafe>();
		State->RequestId = FGuid::NewGuid();
		State->Mode = Mode;
		State->RequestData.ResolvedModel = TEXT("test-model");
		State->RetryPolicy.MaxRetries = 2;
		State->RetryPolicy.InitialDelaySeconds = 60.0f;
		State->RetryPolicy.MaxDelaySeconds = 60.0f;
		Client->ActiveRequests.Add(State->RequestId, State);
		return State;
	};

	FUnrealAIError RetryError;
	RetryError.bIsError = true;
	RetryError.Type = TEXT("transport_error");
	RetryError.Code = TEXT("request_transport_failed");

	const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> RestartState =
		MakeState(UnrealAIClientPrivate::ERequestMode::OneShot);
	const FString StableRequestBody = TEXT("{\"game_request_id\":\"00000000-0000-0000-0000-000000000042\"}");
	RestartState->RequestData.Body = StableRequestBody;
	int32 RetryEventCount = 0;
	FUnrealAIRetryEvent ObservedRetryEvent;
	RestartState->RetryDelegate = FUnrealAIRetryNativeDelegate::CreateLambda(
		[&RetryEventCount, &ObservedRetryEvent](const FUnrealAIRetryEvent &Event)
		{
			++RetryEventCount;
			ObservedRetryEvent = Event;
		});
	int32 StartAttemptCount = 0;
	FString ObservedRetriedBody;
	FUnrealAIRequestState *RestartStatePtr = &RestartState.Get();
	RestartState->StartAttemptForTesting = [&StartAttemptCount, &ObservedRetriedBody, RestartStatePtr]()
	{
		++StartAttemptCount;
		ObservedRetriedBody = RestartStatePtr->RequestData.Body;
	};

	Test.TestTrue(TEXT("A retryable failure enters backoff"),
					   Client->TryScheduleRetry(RestartState, EUnrealAIRetryReason::ConnectionError, 0, RetryError,
												FHttpResponsePtr()));
	Test.TestTrue(TEXT("Backoff marks the request as waiting"), RestartState->bWaitingForRetry);
	Test.TestTrue(TEXT("Backoff owns a cancellable ticker"), RestartState->RetryTickerHandle.IsValid());
	Test.TestEqual(TEXT("Backoff emits one retry event"), RetryEventCount, 1);
	Test.TestEqual(TEXT("The retry event identifies its logical request"), ObservedRetryEvent.RequestHandle.Id,
						RestartState->RequestId);

	UnrealAIClientPrivate::RemoveRetryTicker(RestartState);
	Client->ResumeRequestAfterBackoff(RestartState->RequestId);
	Test.TestEqual(TEXT("Resuming backoff starts exactly one attempt"), StartAttemptCount, 1);
	Test.TestEqual(TEXT("The resumed request advances its attempt number"), RestartState->AttemptNumber, 1);
	Test.TestEqual(TEXT("A retry reuses the originally serialized request body"), ObservedRetriedBody,
						StableRequestBody);
	Test.TestFalse(TEXT("The resumed request is no longer waiting"), RestartState->bWaitingForRetry);

	FUnrealAIRequestHandle RestartHandle;
	RestartHandle.Id = RestartState->RequestId;
	Test.TestTrue(TEXT("The resumed test request can be cancelled"), Client->CancelRequest(RestartHandle));

	const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> WaitingState =
		MakeState(UnrealAIClientPrivate::ERequestMode::OneShot);
	int32 TerminalCount = 0;
	FUnrealAIError TerminalError;
	WaitingState->CompletionDelegate = FUnrealAIChatCompletionNativeDelegate::CreateLambda(
		[&TerminalCount, &TerminalError](const FUnrealAIChatResponse &Response, const FUnrealAIError &Error)
		{
			(void)Response;
			++TerminalCount;
			TerminalError = Error;
		});
	Test.TestTrue(
		TEXT("A second request can wait in backoff"),
			 Client->TryScheduleRetry(WaitingState, EUnrealAIRetryReason::Timeout, 0, RetryError, FHttpResponsePtr()));

	FUnrealAIRequestHandle WaitingHandle;
	WaitingHandle.Id = WaitingState->RequestId;
	Test.TestTrue(TEXT("Cancellation succeeds during backoff"), Client->CancelRequest(WaitingHandle));
	Test.TestEqual(TEXT("Backoff cancellation emits one terminal callback"), TerminalCount, 1);
	Test.TestEqual(TEXT("Backoff cancellation uses the stable cancellation code"), TerminalError.Code,
						FString(TEXT("request_cancelled")));
	Test.TestFalse(TEXT("Backoff cancellation removes its ticker"), WaitingState->RetryTickerHandle.IsValid());
	Test.TestFalse(TEXT("Backoff cancellation removes the active request"),
						Client->ActiveRequests.Contains(WaitingHandle.Id));
	Test.TestFalse(TEXT("A terminal request cannot be cancelled twice"), Client->CancelRequest(WaitingHandle));
	Test.TestEqual(TEXT("A second cancellation does not duplicate completion"), TerminalCount, 1);

	const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> StreamState =
		MakeState(UnrealAIClientPrivate::ERequestMode::Stream);
	StreamState->bSawStreamEvent = true;
	int32 StreamRetryEventCount = 0;
	StreamState->RetryDelegate = FUnrealAIRetryNativeDelegate::CreateLambda(
		[&StreamRetryEventCount](const FUnrealAIRetryEvent &Event)
		{
			(void)Event;
			++StreamRetryEventCount;
		});
	int32 StreamTerminalCount = 0;
	FUnrealAIChatStreamResult StreamResult;
	StreamState->TerminalDelegate = FUnrealAIChatStreamTerminalNativeDelegate::CreateLambda(
		[&StreamTerminalCount, &StreamResult](const FUnrealAIChatStreamResult &Result)
		{
			++StreamTerminalCount;
			StreamResult = Result;
		});
	Test.TestFalse(TEXT("A stream is not replayed after a complete SSE event"),
						Client->TryScheduleRetry(StreamState, EUnrealAIRetryReason::ConnectionError, 0, RetryError,
												 FHttpResponsePtr()));
	Test.TestEqual(TEXT("An ineligible stream emits no retry event"), StreamRetryEventCount, 0);
	Test.TestFalse(TEXT("An ineligible stream creates no retry ticker"), StreamState->RetryTickerHandle.IsValid());

	FUnrealAIRequestHandle StreamHandle;
	StreamHandle.Id = StreamState->RequestId;
	Test.TestTrue(TEXT("The stream test request can be cancelled"), Client->CancelRequest(StreamHandle));
	Test.TestEqual(TEXT("Stream cancellation emits one terminal callback"), StreamTerminalCount, 1);
	Test.TestEqual(TEXT("Stream cancellation remains a distinct terminal status"), StreamResult.Status,
						EUnrealAIChatStreamStatus::Cancelled);
	Test.TestFalse(TEXT("Stream cancellation does not report a failure error"), StreamResult.Error.bIsError);
	for (const UnrealAIClientPrivate::ERequestMode Mode :
		 {UnrealAIClientPrivate::ERequestMode::OneShot, UnrealAIClientPrivate::ERequestMode::Stream})
	{
		const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> ResponseState = MakeState(Mode);
		ResponseState->ResponseState = MakeUnique<FUnrealAIResponseState>();
		ResponseState->RequestData.Body = TEXT("fixture request body");
		ResponseState->RequestData.Headers.Add(TEXT("fixture-header"), TEXT("fixture value"));
		int32 ResponseTerminalCount = 0;
		FUnrealAIResponseResult CancelledResponse;
		ResponseState->ResponseDelegate = FUnrealAIResponseNativeDelegate::CreateLambda(
			[&ResponseTerminalCount, &CancelledResponse](const FUnrealAIResponseResult &Result)
			{
				++ResponseTerminalCount;
				CancelledResponse = Result;
			});
		Test.TestTrue(TEXT("Responses coordinator enters backoff"),
						   Client->TryScheduleRetry(ResponseState, EUnrealAIRetryReason::ConnectionError, 0, RetryError,
													FHttpResponsePtr()));
		FUnrealAIRequestHandle ResponseHandle;
		ResponseHandle.Id = ResponseState->RequestId;
		Test.TestTrue(TEXT("Responses cancellation during backoff"), Client->CancelRequest(ResponseHandle));
		Test.TestEqual(TEXT("Responses terminal once"), ResponseTerminalCount, 1);
		Test.TestEqual(TEXT("Responses cancelled status"), CancelledResponse.Status,
							EUnrealAIResponseStatus::Cancelled);
		Test.TestFalse(TEXT("Responses cancellation empty error"), CancelledResponse.Error.bIsError);
		Test.TestEqual(TEXT("Responses stable logical ID"), CancelledResponse.RequestHandle.Id, ResponseHandle.Id);
		Test.TestFalse(TEXT("Responses timer released"), ResponseState->RetryTickerHandle.IsValid());
		Test.TestFalse(TEXT("Responses HTTP reference released"), ResponseState->HttpRequest.IsValid());
		Test.TestFalse(TEXT("Responses delegate released"), ResponseState->ResponseDelegate.IsBound());
		Test.TestFalse(TEXT("Responses parser state released"), ResponseState->ResponseState.IsValid());
		Test.TestTrue(TEXT("Responses request data cleared"),
						   ResponseState->RequestData.Body.IsEmpty() && ResponseState->RequestData.Headers.IsEmpty());
		Test.TestFalse(TEXT("Responses duplicate cancellation rejected"), Client->CancelRequest(ResponseHandle));
	}
	Test.TestTrue(TEXT("The retry coordinator leaves no active test requests"), Client->ActiveRequests.IsEmpty());
}

#endif

void FUnrealAIExecutionService::Shutdown()
{
	if (DeadlineTicker.IsValid())
	{
		FTSTicker::RemoveTicker(DeadlineTicker);
		DeadlineTicker.Reset();
	}
	CancelAllRequests();
}

FUnrealAIExecutionService::~FUnrealAIExecutionService()
{
	Shutdown();
}

bool FUnrealAIExecutionService::SetLimits(const FUnrealAIExecutionLimits &InLimits)
{
	check(IsInGameThread());
	OutstandingLifetimes.RemoveAll(
		[](const auto &Weak)
		{
			const auto Lifetime = Weak.Pin();
			return !Lifetime || Lifetime->IsPhysicallySettled();
		});
	if (!ActiveRequests.IsEmpty() || !OutstandingLifetimes.IsEmpty() || !InLimits.Validate())
	{
		return false;
	}
	Limits = InLimits;
	return true;
}

bool FUnrealAIExecutionService::ValidateAdmission(FUnrealAIError &OutError) const
{
	if (!FMath::IsFinite(ProviderConfig.TimeoutSeconds) || ProviderConfig.TimeoutSeconds <= 0.0f ||
		ProviderConfig.TimeoutSeconds > 1800.0f)
	{
		OutError = UnrealAIClientPrivate::MakeRequestError(
			TEXT("Request timeout must be finite and between zero and 1800 seconds."), TEXT("invalid_request_error"),
																							TEXT("invalid_deadline"));
		return false;
	}
	OutstandingLifetimes.RemoveAll(
		[](const TWeakPtr<const IUnrealAIRequestLifetime, ESPMode::ThreadSafe> &Weak)
		{
			const TSharedPtr<const IUnrealAIRequestLifetime, ESPMode::ThreadSafe> Lifetime = Weak.Pin();
			return !Lifetime || Lifetime->IsPhysicallySettled();
		});
	if (OutstandingLifetimes.Num() >= Limits.MaxConcurrentRequests)
	{
		OutError = UnrealAIClientPrivate::MakeRequestError(TEXT("The execution service has reached its request limit."),
																TEXT("admission_error"), TEXT("request_capacity"));
		return false;
	}
#if UE_BUILD_SHIPPING && !UE_SERVER
	if (!ResolveApiKey().IsEmpty() || ProviderConfig.bRequiresApiKey)
	{
		OutError = UnrealAIClientPrivate::MakeRequestError(
			TEXT("Shipping clients must use a trusted backend for provider credentials."),
				 TEXT("configuration_error"), TEXT("client_provider_credentials_forbidden"));
		return false;
	}
#endif
	return true;
}

void FUnrealAIExecutionService::EnsureDeadlineTicker()
{
	if (DeadlineTicker.IsValid())
	{
		return;
	}
	TWeakPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> Weak = AsShared();
	DeadlineTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Weak](float DeltaSeconds)
		{
			if (TSharedPtr<FUnrealAIExecutionService, ESPMode::ThreadSafe> Service = Weak.Pin())
			{
				return Service->PumpDeadlines(DeltaSeconds);
			}
			return false;
		}));
}

bool FUnrealAIExecutionService::PumpDeadlines(float DeltaSeconds)
{
	(void)DeltaSeconds;
	TArray<TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>> States;
	ActiveRequests.GenerateValueArray(States);
	const double Now = FPlatformTime::Seconds();
	for (const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> &State : States)
	{
		if (!State->bTerminal && State->DeadlineSeconds > 0.0 && Now >= State->DeadlineSeconds)
		{
			const FHttpRequestPtr Http = State->HttpRequest;
			const FUnrealAIError Error =
				UnrealAIClientPrivate::MakeRequestError(TEXT("The model request reached its end-to-end deadline."),
															 TEXT("timeout_error"), TEXT("request_deadline"));
			if (State->Mode == UnrealAIClientPrivate::ERequestMode::Stream)
			{
				CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, Error);
			}
			else
			{
				CompleteOneShotRequest(State, {}, Error);
			}
			if (Http)
			{
				Http->CancelRequest();
			}
		}
	}
	if (ActiveRequests.IsEmpty())
	{
		DeadlineTicker.Reset();
		return false;
	}
	return true;
}
