#include "UnrealAIClient.h"

#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ScopeLock.h"
#include "UnrealAIProviderAdapter.h"
#include "UnrealAIRetryPolicy.h"
#include "UnrealAISettings.h"
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

	FString Utf8BytesToString(const TArray<uint8>& Bytes)
	{
		if (Bytes.IsEmpty())
		{
			return FString();
		}

		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	FUnrealAIError MakeRequestError(
		const FString& Message,
		const FString& Type,
		const FString& Code,
		int32 HttpStatus = 0)
	{
		FUnrealAIError Error = UnrealAIProviderAdapters::MakeError(Message, HttpStatus);
		Error.Type = Type;
		Error.Code = Code;
		return Error;
	}

	FUnrealAIError MakeStreamError(const FString& Message, const FString& Code, int32 HttpStatus = 0)
	{
		return MakeRequestError(Message, TEXT("stream_error"), Code, HttpStatus);
	}

	FUnrealAIError MakeCancellationError()
	{
		return MakeRequestError(
			TEXT("The UnrealAI request was cancelled."),
			TEXT("request_cancelled"),
			TEXT("request_cancelled"));
	}

	EUnrealAIRetryReason GetTransportRetryReason(const FHttpRequestPtr& HttpRequest)
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
}

struct FUnrealAIRequestState
{
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
#if WITH_DEV_AUTOMATION_TESTS
	TFunction<void()> StartAttemptForTesting;
#endif
};

namespace UnrealAIClientPrivate
{
	void DetachHttpRequest(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State)
	{
		if (!State.IsValid() || !State->HttpRequest.IsValid())
		{
			return;
		}

		State->HttpRequest->OnStatusCodeReceived().Unbind();
		State->HttpRequest->OnProcessRequestComplete().Unbind();
		State->HttpRequest.Reset();
	}

	void RemoveRetryTicker(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State)
	{
		if (State.IsValid() && State->RetryTickerHandle.IsValid())
		{
			FTSTicker::RemoveTicker(State->RetryTickerHandle);
			State->RetryTickerHandle.Reset();
		}
	}

	void ResetStreamAttempt(
		const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
		bool bAcceptData)
	{
		{
			const FScopeLock Lock(&State->QueueMutex);
			State->PendingChunks.Reset();
			State->bDrainScheduled = false;
			State->bAcceptData = bAcceptData;
			State->bQueueOverflowed = false;
			State->HttpStatus = 0;
		}
		State->ErrorBodyBytes.Reset();
		State->SseParser = FUnrealAISseParser();
		State->ProviderState = FUnrealAIProviderStreamState();
		State->ProviderState.Response.Model = State->RequestData.ResolvedModel;
		State->ProviderState.ExpectedChoiceCount = State->ExpectedChoiceCount;
	}

	void ConfigureHttpRequest(
		const FHttpRequestRef& HttpRequest,
		const FUnrealAIRequestState& State)
	{
		HttpRequest->SetURL(State.RequestData.Url);
		HttpRequest->SetVerb(State.RequestData.Verb);
		if (State.Mode == ERequestMode::Stream)
		{
			HttpRequest->SetTimeout(0.0f);
			HttpRequest->SetActivityTimeout(FMath::Max(1.0f, State.TimeoutSeconds));
		}
		else
		{
			HttpRequest->SetTimeout(FMath::Max(1.0f, State.TimeoutSeconds));
		}

		for (const TPair<FString, FString>& Header : State.RequestData.Headers)
		{
			HttpRequest->SetHeader(Header.Key, Header.Value);
		}
		HttpRequest->SetContentAsString(State.RequestData.Body);
	}
}

void UUnrealAIClient::Configure(const FUnrealAIProviderConfig& InProviderConfig)
{
	ProviderConfig = InProviderConfig;
	bConfigured = true;
}

bool UUnrealAIClient::ConfigureFromSettings(FName ProviderName, FUnrealAIError& OutError)
{
	const UUnrealAISettings* Settings = GetDefault<UUnrealAISettings>();
	if (!Settings)
	{
		OutError = UnrealAIProviderAdapters::MakeError(TEXT("UnrealAI settings are unavailable."));
		return false;
	}

	FUnrealAIProviderConfig ResolvedProvider;
	if (!Settings->TryGetProviderConfig(ProviderName, ResolvedProvider))
	{
		const FString ResolvedName = ProviderName.IsNone() ? Settings->DefaultProviderName.ToString() : ProviderName.ToString();
		OutError = UnrealAIProviderAdapters::MakeError(
			FString::Printf(TEXT("Provider profile '%s' was not found."), *ResolvedName));
		return false;
	}

	Configure(ResolvedProvider);
	OutError = FUnrealAIError();
	return true;
}

bool UUnrealAIClient::IsConfigured() const
{
	return bConfigured;
}

const FUnrealAIProviderConfig& UUnrealAIClient::GetProviderConfig() const
{
	return ProviderConfig;
}

FUnrealAIRequestHandle UUnrealAIClient::CreateChatCompletion(
	const FUnrealAIChatRequest& Request,
	FUnrealAIChatCompletionNativeDelegate CompletionDelegate,
	FUnrealAIRetryNativeDelegate RetryDelegate)
{
	check(IsInGameThread());
	auto FailBeforeStart = [&CompletionDelegate](const FUnrealAIError& Error)
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(EmptyResponse, Error);
	};

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
			FString::Printf(
				TEXT("API key is not configured. Set %s or provide an API key override."),
				*ProviderConfig.ApiKeyEnvironmentVariable)));
		return FUnrealAIRequestHandle();
	}

	const IUnrealAIProviderAdapter& Adapter = UnrealAIProviderAdapters::Get(ProviderConfig.Api);
	FUnrealAIHttpRequestData RequestData;
	FUnrealAIError BuildError;
	if (!Adapter.BuildRequest(
		ProviderConfig,
		Request,
		ApiKey,
		EUnrealAIRequestMode::OneShot,
		RequestData,
		BuildError))
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
	State->RetryPolicy = UnrealAIRetryPolicy::Resolve(ProviderConfig.RetryPolicy, Request.RetryOptions);
	State->CompletionDelegate = MoveTemp(CompletionDelegate);
	State->RetryDelegate = MoveTemp(RetryDelegate);

	ActiveRequests.Add(State->RequestId, State);
	StartRequestAttempt(State);

	FUnrealAIRequestHandle Handle;
	if (ActiveRequests.Contains(State->RequestId))
	{
		Handle.Id = State->RequestId;
	}
	return Handle;
}

FUnrealAIRequestHandle UUnrealAIClient::StreamChatCompletion(
	const FUnrealAIChatRequest& Request,
	FUnrealAIChatStreamEventNativeDelegate EventDelegate,
	FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate,
	FUnrealAIRetryNativeDelegate RetryDelegate)
{
	check(IsInGameThread());
	auto FailBeforeStart = [&TerminalDelegate](const FUnrealAIError& Error)
	{
		FUnrealAIChatStreamResult Result;
		Result.Status = EUnrealAIChatStreamStatus::Failed;
		Result.Error = Error;
		TerminalDelegate.ExecuteIfBound(Result);
	};

	if (!bConfigured)
	{
		FailBeforeStart(UnrealAIProviderAdapters::MakeError(TEXT("Client is not configured.")));
		return FUnrealAIRequestHandle();
	}

	const FString ApiKey = ResolveApiKey();
	if (ProviderConfig.bRequiresApiKey && ApiKey.IsEmpty())
	{
		FailBeforeStart(UnrealAIProviderAdapters::MakeError(
			FString::Printf(
				TEXT("API key is not configured. Set %s or provide an API key override."),
				*ProviderConfig.ApiKeyEnvironmentVariable)));
		return FUnrealAIRequestHandle();
	}

	const IUnrealAIProviderAdapter& Adapter = UnrealAIProviderAdapters::Get(ProviderConfig.Api);
	FUnrealAIHttpRequestData RequestData;
	FUnrealAIError BuildError;
	if (!Adapter.BuildRequest(
		ProviderConfig,
		Request,
		ApiKey,
		EUnrealAIRequestMode::Stream,
		RequestData,
		BuildError))
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
	State->RetryPolicy = UnrealAIRetryPolicy::Resolve(ProviderConfig.RetryPolicy, Request.RetryOptions);
	State->ExpectedChoiceCount = FMath::Max(1, Request.NumChoices);
	State->EventDelegate = MoveTemp(EventDelegate);
	State->TerminalDelegate = MoveTemp(TerminalDelegate);
	State->RetryDelegate = MoveTemp(RetryDelegate);

	ActiveRequests.Add(State->RequestId, State);
	StartRequestAttempt(State);

	FUnrealAIRequestHandle Handle;
	if (ActiveRequests.Contains(State->RequestId))
	{
		Handle.Id = State->RequestId;
	}
	return Handle;
}

void UUnrealAIClient::StartRequestAttempt(
	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State)
{
	if (!State.IsValid() || State->bTerminal || !ActiveRequests.Contains(State->RequestId))
	{
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

	if (State->Mode == UnrealAIClientPrivate::ERequestMode::OneShot)
	{
		HttpRequest->OnProcessRequestComplete().BindUObject(
			this,
			&UUnrealAIClient::HandleChatCompletionResponse,
			State->RequestId,
			AttemptNumber);
	}
	else
	{
		TWeakObjectPtr<UUnrealAIClient> WeakThis(this);
		TWeakPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> WeakState = State;
		FHttpRequestStreamDelegateV2 StreamDelegate;
		StreamDelegate.BindLambda([WeakState, WeakThis, AttemptNumber](void* Data, int64& Length)
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

				if (!PinnedState->PendingChunks.Enqueue(static_cast<const uint8*>(Data), Length))
				{
					PinnedState->bAcceptData = false;
					PinnedState->bQueueOverflowed = true;
					Length = 0;
				}
				if (!PinnedState->bDrainScheduled)
				{
					PinnedState->bDrainScheduled = true;
					bScheduleDrain = true;
				}
			}

			if (bScheduleDrain)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestId = PinnedState->RequestId]()
				{
					if (UUnrealAIClient* Client = WeakThis.Get())
					{
						Client->DrainStreamRequest(RequestId);
					}
				});
			}
		});
		if (!HttpRequest->SetResponseBodyReceiveStreamDelegateV2(StreamDelegate))
		{
			UnrealAIClientPrivate::DetachHttpRequest(State);
			CompleteStreamRequest(
				State,
				EUnrealAIChatStreamStatus::Failed,
				UnrealAIClientPrivate::MakeStreamError(
					TEXT("The active Unreal HTTP backend does not support response streaming."),
					TEXT("stream_transport_unavailable")));
			return;
		}

		HttpRequest->OnStatusCodeReceived().BindLambda([WeakState, WeakThis, AttemptNumber](FHttpRequestPtr RequestPtr, int32 StatusCode)
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
				if (PinnedState->HttpStatus != 0 && !PinnedState->PendingChunks.IsEmpty() && !PinnedState->bDrainScheduled)
				{
					PinnedState->bDrainScheduled = true;
					bScheduleDrain = true;
				}
			}

			if (bScheduleDrain)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestId = PinnedState->RequestId]()
				{
					if (UUnrealAIClient* Client = WeakThis.Get())
					{
						Client->DrainStreamRequest(RequestId);
					}
				});
			}
		});
		HttpRequest->OnProcessRequestComplete().BindUObject(
			this,
			&UUnrealAIClient::HandleStreamResponse,
			State->RequestId,
			AttemptNumber);
	}

	if (!HttpRequest->ProcessRequest())
	{
		UnrealAIClientPrivate::DetachHttpRequest(State);
		if (State->Mode == UnrealAIClientPrivate::ERequestMode::Stream)
		{
			CompleteStreamRequest(
				State,
				EUnrealAIChatStreamStatus::Failed,
				UnrealAIClientPrivate::MakeStreamError(
					TEXT("Failed to start streaming HTTP request."),
					TEXT("stream_start_failed")));
		}
		else
		{
			FUnrealAIChatResponse EmptyResponse;
			CompleteOneShotRequest(
				State,
				EmptyResponse,
				UnrealAIClientPrivate::MakeRequestError(
					TEXT("Failed to start HTTP request."),
					TEXT("transport_error"),
					TEXT("request_start_failed")));
		}
	}
}

void UUnrealAIClient::ResumeRequestAfterBackoff(const FGuid& RequestId)
{
	if (TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>* StatePtr = ActiveRequests.Find(RequestId))
	{
		StartRequestAttempt(*StatePtr);
	}
}

bool UUnrealAIClient::CancelRequest(const FUnrealAIRequestHandle& RequestHandle)
{
	check(IsInGameThread());
	if (!RequestHandle.IsValid())
	{
		return false;
	}

	TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>* StatePtr = ActiveRequests.Find(RequestHandle.Id);
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

FString UUnrealAIClient::ResolveApiKey() const
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

void UUnrealAIClient::HandleChatCompletionResponse(
	FHttpRequestPtr HttpRequest,
	FHttpResponsePtr HttpResponse,
	bool bWasSuccessful,
	FGuid RequestId,
	int32 AttemptNumber)
{
	TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>* StatePtr = ActiveRequests.Find(RequestId);
	if (!StatePtr || (*StatePtr)->bTerminal || (*StatePtr)->AttemptNumber != AttemptNumber)
	{
		return;
	}

	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe> State = *StatePtr;
	UnrealAIClientPrivate::DetachHttpRequest(State);

	FUnrealAIChatResponse ParsedResponse;
	FUnrealAIError Error;
	if (!bWasSuccessful || !HttpResponse.IsValid())
	{
		Error = UnrealAIClientPrivate::MakeRequestError(
			TEXT("HTTP request failed before a provider response was received."),
			TEXT("transport_error"),
			TEXT("request_transport_failed"));
		const EUnrealAIRetryReason Reason = UnrealAIClientPrivate::GetTransportRetryReason(HttpRequest);
		if (TryScheduleRetry(State, Reason, 0, Error, HttpResponse))
		{
			return;
		}
		CompleteOneShotRequest(State, ParsedResponse, Error);
		return;
	}

	const IUnrealAIProviderAdapter& Adapter = UnrealAIProviderAdapters::Get(State->ProviderApi);
	Adapter.ParseResponse(
		State->RequestData.ResolvedModel,
		HttpResponse->GetResponseCode(),
		HttpResponse->GetContentAsString(),
		ParsedResponse,
		Error);
	if (Error.bIsError && TryScheduleRetry(
		State,
		EUnrealAIRetryReason::HttpError,
		HttpResponse->GetResponseCode(),
		Error,
		HttpResponse))
	{
		return;
	}
	CompleteOneShotRequest(State, ParsedResponse, Error);
}

void UUnrealAIClient::DrainStreamRequest(const FGuid& RequestId)
{
	TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>* StatePtr = ActiveRequests.Find(RequestId);
	if (!StatePtr
		|| (*StatePtr)->bTerminal
		|| (*StatePtr)->Mode != UnrealAIClientPrivate::ERequestMode::Stream
		|| (*StatePtr)->bWaitingForRetry)
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
		CompleteStreamRequest(
			State,
			EUnrealAIChatStreamStatus::Failed,
			UnrealAIClientPrivate::MakeStreamError(
				TEXT("The HTTP stream exceeded the 4 MiB pending-data safety limit."),
				TEXT("stream_buffer_overflow"),
				HttpStatus));
		if (HttpRequest.IsValid())
		{
			HttpRequest->CancelRequest();
		}
		return;
	}

	for (const TArray<uint8>& Chunk : Chunks)
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

		for (const FUnrealAISseEvent& SseEvent : SseEvents)
		{
			State->bSawStreamEvent = true;
			TArray<FUnrealAIChatStreamEvent> Events;
			if (!UnrealAIProviderAdapters::Get(State->ProviderApi).ParseStreamEvent(
				SseEvent, State->ProviderState, Events, ParseError))
			{
				const FHttpRequestPtr HttpRequest = State->HttpRequest;
				CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ParseError);
				if (HttpRequest.IsValid())
				{
					HttpRequest->CancelRequest();
				}
				return;
			}

			for (const FUnrealAIChatStreamEvent& Event : Events)
			{
				State->EventDelegate.ExecuteIfBound(Event);
				if (!ActiveRequests.Contains(RequestId))
				{
					return;
				}
			}
		}
	}
}

void UUnrealAIClient::HandleStreamResponse(
	FHttpRequestPtr HttpRequest,
	FHttpResponsePtr HttpResponse,
	bool bWasSuccessful,
	FGuid RequestId,
	int32 AttemptNumber)
{
	TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>* StatePtr = ActiveRequests.Find(RequestId);
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
			TEXT("stream_transport_failed"),
			HttpStatus);
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
		UnrealAIProviderAdapters::Get(State->ProviderApi).ParseResponse(
			State->RequestData.ResolvedModel,
			HttpStatus,
			UnrealAIClientPrivate::Utf8BytesToString(State->ErrorBodyBytes),
			IgnoredResponse,
			ProviderError);
		if (!State->bSawStreamEvent && TryScheduleRetry(
			State,
			EUnrealAIRetryReason::HttpError,
			HttpStatus,
			ProviderError,
			HttpResponse))
		{
			return;
		}
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ProviderError);
		return;
	}

	const FString ContentType = HttpResponse->GetContentType();
	if (!ContentType.Contains(TEXT("text/event-stream"), ESearchCase::IgnoreCase))
	{
		CompleteStreamRequest(
			State,
			EUnrealAIChatStreamStatus::Failed,
			UnrealAIClientPrivate::MakeStreamError(
				FString::Printf(TEXT("Provider returned '%s' instead of text/event-stream."), *ContentType),
				TEXT("invalid_stream_content_type"),
				HttpStatus));
		return;
	}

	TArray<FUnrealAISseEvent> FinalSseEvents;
	FUnrealAIError ParseError;
	if (!State->SseParser.Finish(FinalSseEvents, ParseError))
	{
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ParseError);
		return;
	}
	for (const FUnrealAISseEvent& SseEvent : FinalSseEvents)
	{
		State->bSawStreamEvent = true;
		TArray<FUnrealAIChatStreamEvent> Events;
		if (!UnrealAIProviderAdapters::Get(State->ProviderApi).ParseStreamEvent(
			SseEvent, State->ProviderState, Events, ParseError))
		{
			CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, ParseError);
			return;
		}
		for (const FUnrealAIChatStreamEvent& Event : Events)
		{
			State->EventDelegate.ExecuteIfBound(Event);
			if (!ActiveRequests.Contains(RequestId))
			{
				return;
			}
		}
	}

	if (!UnrealAIProviderAdapters::Get(State->ProviderApi).CanCompleteStream(State->ProviderState))
	{
		const FUnrealAIError Error = UnrealAIClientPrivate::MakeStreamError(
			TEXT("The provider stream ended without its required terminal event."),
			TEXT("truncated_stream"),
			HttpStatus);
		if (!State->bSawStreamEvent && TryScheduleRetry(
			State,
			EUnrealAIRetryReason::EmptyStream,
			HttpStatus,
			Error,
			HttpResponse))
		{
			return;
		}
		CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Failed, Error);
		return;
	}

	FUnrealAIError NoError;
	CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Completed, NoError);
}

bool UUnrealAIClient::TryScheduleRetry(
	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
	EUnrealAIRetryReason Reason,
	int32 HttpStatus,
	const FUnrealAIError& Error,
	const FHttpResponsePtr& HttpResponse)
{
	if (!State.IsValid()
		|| State->bTerminal
		|| !ActiveRequests.Contains(State->RequestId)
		|| (State->Mode == UnrealAIClientPrivate::ERequestMode::Stream
			&& !UnrealAIRetryPolicy::CanRetryStream(State->bSawStreamEvent)))
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
	if (!UnrealAIRetryPolicy::TryComputeDelay(
		State->RetryPolicy,
		RetryNumber,
		Failure.RetryAfter,
		FDateTime::UtcNow(),
		FMath::FRand(),
		DelaySeconds))
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

	TWeakObjectPtr<UUnrealAIClient> WeakThis(this);
	State->RetryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("UnrealAI request retry"),
		DelaySeconds,
		[WeakThis, RequestId = State->RequestId](float DeltaSeconds)
		{
			(void)DeltaSeconds;
			if (UUnrealAIClient* Client = WeakThis.Get())
			{
				Client->ResumeRequestAfterBackoff(RequestId);
			}
			return false;
		});
	return true;
}

void UUnrealAIClient::CompleteOneShotRequest(
	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
	const FUnrealAIChatResponse& Response,
	const FUnrealAIError& Error)
{
	if (!State.IsValid() || State->bTerminal)
	{
		return;
	}

	State->bTerminal = true;
	UnrealAIClientPrivate::RemoveRetryTicker(State);
	UnrealAIClientPrivate::DetachHttpRequest(State);
	ActiveRequests.Remove(State->RequestId);
	State->RequestData.Headers.Reset();
	State->RequestData.Body.Reset();

	State->CompletionDelegate.ExecuteIfBound(Response, Error);
	State->CompletionDelegate.Unbind();
	State->RetryDelegate.Unbind();
}

void UUnrealAIClient::CompleteStreamRequest(
	const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
	EUnrealAIChatStreamStatus Status,
	const FUnrealAIError& Error)
{
	if (!State.IsValid() || State->bTerminal)
	{
		return;
	}

	State->bTerminal = true;
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
	State->ProviderState.Response.Choices.Sort(
		[](const FUnrealAIChatChoice& Left, const FUnrealAIChatChoice& Right)
		{
			return Left.Index < Right.Index;
		});

	FUnrealAIChatStreamResult Result;
	Result.Status = Status;
	Result.Response = State->ProviderState.Response;
	Result.Error = Error;
	State->TerminalDelegate.ExecuteIfBound(Result);
	State->EventDelegate.Unbind();
	State->TerminalDelegate.Unbind();
	State->RetryDelegate.Unbind();
}

void UUnrealAIClient::CancelAllRequests()
{
	TArray<TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>> States;
	ActiveRequests.GenerateValueArray(States);
	ActiveRequests.Reset();
	for (const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State : States)
	{
		if (!State.IsValid())
		{
			continue;
		}

		State->bTerminal = true;
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

void FUnrealAIClientTestAccess::RunRetryCoordinatorTests(FAutomationTestBase& Test)
{
	UUnrealAIClient* Client = NewObject<UUnrealAIClient>();
	Test.TestNotNull(TEXT("The retry coordinator test client is created"), Client);
	if (!Client)
	{
		return;
	}

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
	const FString StableRequestBody =
		TEXT("{\"game_request_id\":\"00000000-0000-0000-0000-000000000042\"}");
	RestartState->RequestData.Body = StableRequestBody;
	int32 RetryEventCount = 0;
	FUnrealAIRetryEvent ObservedRetryEvent;
	RestartState->RetryDelegate = FUnrealAIRetryNativeDelegate::CreateLambda(
		[&RetryEventCount, &ObservedRetryEvent](const FUnrealAIRetryEvent& Event)
		{
			++RetryEventCount;
			ObservedRetryEvent = Event;
		});
	int32 StartAttemptCount = 0;
	FString ObservedRetriedBody;
	FUnrealAIRequestState* RestartStatePtr = &RestartState.Get();
	RestartState->StartAttemptForTesting =
		[&StartAttemptCount, &ObservedRetriedBody, RestartStatePtr]()
	{
		++StartAttemptCount;
		ObservedRetriedBody = RestartStatePtr->RequestData.Body;
	};

	Test.TestTrue(
		TEXT("A retryable failure enters backoff"),
		Client->TryScheduleRetry(
			RestartState,
			EUnrealAIRetryReason::ConnectionError,
			0,
			RetryError,
			FHttpResponsePtr()));
	Test.TestTrue(TEXT("Backoff marks the request as waiting"), RestartState->bWaitingForRetry);
	Test.TestTrue(TEXT("Backoff owns a cancellable ticker"), RestartState->RetryTickerHandle.IsValid());
	Test.TestEqual(TEXT("Backoff emits one retry event"), RetryEventCount, 1);
	Test.TestEqual(
		TEXT("The retry event identifies its logical request"),
		ObservedRetryEvent.RequestHandle.Id,
		RestartState->RequestId);

	UnrealAIClientPrivate::RemoveRetryTicker(RestartState);
	Client->ResumeRequestAfterBackoff(RestartState->RequestId);
	Test.TestEqual(TEXT("Resuming backoff starts exactly one attempt"), StartAttemptCount, 1);
	Test.TestEqual(TEXT("The resumed request advances its attempt number"), RestartState->AttemptNumber, 1);
	Test.TestEqual(
		TEXT("A retry reuses the originally serialized request body"),
		ObservedRetriedBody,
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
		[&TerminalCount, &TerminalError](const FUnrealAIChatResponse& Response, const FUnrealAIError& Error)
		{
			(void)Response;
			++TerminalCount;
			TerminalError = Error;
		});
	Test.TestTrue(
		TEXT("A second request can wait in backoff"),
		Client->TryScheduleRetry(
			WaitingState,
			EUnrealAIRetryReason::Timeout,
			0,
			RetryError,
			FHttpResponsePtr()));

	FUnrealAIRequestHandle WaitingHandle;
	WaitingHandle.Id = WaitingState->RequestId;
	Test.TestTrue(TEXT("Cancellation succeeds during backoff"), Client->CancelRequest(WaitingHandle));
	Test.TestEqual(TEXT("Backoff cancellation emits one terminal callback"), TerminalCount, 1);
	Test.TestEqual(
		TEXT("Backoff cancellation uses the stable cancellation code"),
		TerminalError.Code,
		FString(TEXT("request_cancelled")));
	Test.TestFalse(TEXT("Backoff cancellation removes its ticker"), WaitingState->RetryTickerHandle.IsValid());
	Test.TestFalse(TEXT("Backoff cancellation removes the active request"), Client->ActiveRequests.Contains(WaitingHandle.Id));
	Test.TestFalse(TEXT("A terminal request cannot be cancelled twice"), Client->CancelRequest(WaitingHandle));
	Test.TestEqual(TEXT("A second cancellation does not duplicate completion"), TerminalCount, 1);

	const TSharedRef<FUnrealAIRequestState, ESPMode::ThreadSafe> StreamState =
		MakeState(UnrealAIClientPrivate::ERequestMode::Stream);
	StreamState->bSawStreamEvent = true;
	int32 StreamRetryEventCount = 0;
	StreamState->RetryDelegate = FUnrealAIRetryNativeDelegate::CreateLambda(
		[&StreamRetryEventCount](const FUnrealAIRetryEvent& Event)
		{
			(void)Event;
			++StreamRetryEventCount;
		});
	int32 StreamTerminalCount = 0;
	FUnrealAIChatStreamResult StreamResult;
	StreamState->TerminalDelegate = FUnrealAIChatStreamTerminalNativeDelegate::CreateLambda(
		[&StreamTerminalCount, &StreamResult](const FUnrealAIChatStreamResult& Result)
		{
			++StreamTerminalCount;
			StreamResult = Result;
		});
	Test.TestFalse(
		TEXT("A stream is not replayed after a complete SSE event"),
		Client->TryScheduleRetry(
			StreamState,
			EUnrealAIRetryReason::ConnectionError,
			0,
			RetryError,
			FHttpResponsePtr()));
	Test.TestEqual(TEXT("An ineligible stream emits no retry event"), StreamRetryEventCount, 0);
	Test.TestFalse(TEXT("An ineligible stream creates no retry ticker"), StreamState->RetryTickerHandle.IsValid());

	FUnrealAIRequestHandle StreamHandle;
	StreamHandle.Id = StreamState->RequestId;
	Test.TestTrue(TEXT("The stream test request can be cancelled"), Client->CancelRequest(StreamHandle));
	Test.TestEqual(TEXT("Stream cancellation emits one terminal callback"), StreamTerminalCount, 1);
	Test.TestEqual(
		TEXT("Stream cancellation remains a distinct terminal status"),
		StreamResult.Status,
		EUnrealAIChatStreamStatus::Cancelled);
	Test.TestFalse(TEXT("Stream cancellation does not report a failure error"), StreamResult.Error.bIsError);
	Test.TestTrue(TEXT("The retry coordinator leaves no active test requests"), Client->ActiveRequests.IsEmpty());
}

#endif

void UUnrealAIClient::BeginDestroy()
{
	CancelAllRequests();
	Super::BeginDestroy();
}
