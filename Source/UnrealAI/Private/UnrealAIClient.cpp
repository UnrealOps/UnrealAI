#include "UnrealAIClient.h"

#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ScopeLock.h"
#include "UnrealAIProviderAdapter.h"
#include "UnrealAISettings.h"
#include "UnrealAIStreamChunkQueue.h"

namespace UnrealAIClientPrivate
{
	constexpr int32 MaxErrorBodyBytes = 1024 * 1024;

	FString Utf8BytesToString(const TArray<uint8>& Bytes)
	{
		if (Bytes.IsEmpty())
		{
			return FString();
		}

		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	FUnrealAIError MakeStreamError(const FString& Message, const FString& Code, int32 HttpStatus = 0)
	{
		FUnrealAIError Error = UnrealAIProviderAdapters::MakeError(Message, HttpStatus);
		Error.Type = TEXT("stream_error");
		Error.Code = Code;
		return Error;
	}
}

struct FUnrealAIStreamRequestState
{
	FCriticalSection QueueMutex;
	FUnrealAIStreamChunkQueue PendingChunks;
	TArray<uint8> ErrorBodyBytes;
	bool bDrainScheduled = false;
	bool bAcceptData = true;
	bool bQueueOverflowed = false;
	int32 HttpStatus = 0;

	FGuid RequestId;
	FHttpRequestPtr HttpRequest;
	EUnrealAIProviderApi ProviderApi = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
	FString ResolvedModel;
	FUnrealAISseParser SseParser;
	FUnrealAIProviderStreamState ProviderState;
	FUnrealAIChatStreamEventNativeDelegate EventDelegate;
	FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate;
	bool bTerminal = false;
};

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

void UUnrealAIClient::CreateChatCompletion(
	const FUnrealAIChatRequest& Request,
	FUnrealAIChatCompletionNativeDelegate CompletionDelegate)
{
	if (!bConfigured)
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(
			EmptyResponse,
			UnrealAIProviderAdapters::MakeError(TEXT("Client is not configured.")));
		return;
	}

	if (Request.bStream)
	{
		FUnrealAIChatResponse EmptyResponse;
		FUnrealAIError Error = UnrealAIProviderAdapters::MakeError(
			TEXT("The bStream request field is deprecated. Use StreamChatCompletion or the Stream Chat Completion Blueprint node."));
		Error.Type = TEXT("invalid_request_error");
		Error.Code = TEXT("deprecated_stream_flag");
		CompletionDelegate.ExecuteIfBound(EmptyResponse, Error);
		return;
	}

	const FString ApiKey = ResolveApiKey();
	if (ProviderConfig.bRequiresApiKey && ApiKey.IsEmpty())
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(
			EmptyResponse,
			UnrealAIProviderAdapters::MakeError(
				FString::Printf(
					TEXT("API key is not configured. Set %s or provide an API key override."),
					*ProviderConfig.ApiKeyEnvironmentVariable)));
		return;
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
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(EmptyResponse, BuildError);
		return;
	}

	FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(RequestData.Url);
	HttpRequest->SetVerb(RequestData.Verb);
	HttpRequest->SetTimeout(FMath::Max(1.0f, ProviderConfig.TimeoutSeconds));
	for (const TPair<FString, FString>& Header : RequestData.Headers)
	{
		HttpRequest->SetHeader(Header.Key, Header.Value);
	}
	HttpRequest->SetContentAsString(RequestData.Body);
	HttpRequest->OnProcessRequestComplete().BindUObject(
		this,
		&UUnrealAIClient::HandleChatCompletionResponse,
		ProviderConfig.Api,
		RequestData.ResolvedModel,
		CompletionDelegate);

	InFlightRequests.Add(HttpRequest);
	if (!HttpRequest->ProcessRequest())
	{
		InFlightRequests.Remove(HttpRequest);

		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(
			EmptyResponse,
			UnrealAIProviderAdapters::MakeError(TEXT("Failed to start HTTP request.")));
	}
}

FUnrealAIRequestHandle UUnrealAIClient::StreamChatCompletion(
	const FUnrealAIChatRequest& Request,
	FUnrealAIChatStreamEventNativeDelegate EventDelegate,
	FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate)
{
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

	FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(RequestData.Url);
	HttpRequest->SetVerb(RequestData.Verb);
	HttpRequest->SetTimeout(0.0f);
	HttpRequest->SetActivityTimeout(FMath::Max(1.0f, ProviderConfig.TimeoutSeconds));
	for (const TPair<FString, FString>& Header : RequestData.Headers)
	{
		HttpRequest->SetHeader(Header.Key, Header.Value);
	}
	HttpRequest->SetContentAsString(RequestData.Body);

	const TSharedRef<FUnrealAIStreamRequestState, ESPMode::ThreadSafe> State =
		MakeShared<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>();
	State->RequestId = FGuid::NewGuid();
	State->HttpRequest = HttpRequest;
	State->ProviderApi = ProviderConfig.Api;
	State->ResolvedModel = RequestData.ResolvedModel;
	State->ProviderState.Response.Model = RequestData.ResolvedModel;
	State->ProviderState.ExpectedChoiceCount = FMath::Max(1, Request.NumChoices);
	State->EventDelegate = MoveTemp(EventDelegate);
	State->TerminalDelegate = MoveTemp(TerminalDelegate);
	auto FailStateBeforeStart = [&State](const FUnrealAIError& Error)
	{
		FUnrealAIChatStreamResult Result;
		Result.Status = EUnrealAIChatStreamStatus::Failed;
		Result.Error = Error;
		State->TerminalDelegate.ExecuteIfBound(Result);
		State->EventDelegate.Unbind();
		State->TerminalDelegate.Unbind();
	};

	TWeakObjectPtr<UUnrealAIClient> WeakThis(this);
	TWeakPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe> WeakState = State;
	FHttpRequestStreamDelegateV2 StreamDelegate;
	StreamDelegate.BindLambda([WeakState, WeakThis](void* Data, int64& Length)
	{
		if (!Data || Length <= 0 || Length > MAX_int32)
		{
			return;
		}
		const TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe> State = WeakState.Pin();
		if (!State.IsValid())
		{
			Length = 0;
			return;
		}

		bool bScheduleDrain = false;
		{
			const FScopeLock Lock(&State->QueueMutex);
			if (!State->bAcceptData)
			{
				return;
			}

			if (!State->PendingChunks.Enqueue(static_cast<const uint8*>(Data), Length))
			{
				State->bAcceptData = false;
				State->bQueueOverflowed = true;
				Length = 0;
			}
			if (!State->bDrainScheduled)
			{
				State->bDrainScheduled = true;
				bScheduleDrain = true;
			}
		}

		if (bScheduleDrain)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestId = State->RequestId]()
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
		FailStateBeforeStart(UnrealAIClientPrivate::MakeStreamError(
			TEXT("The active Unreal HTTP backend does not support response streaming."),
			TEXT("stream_transport_unavailable")));
		return FUnrealAIRequestHandle();
	}

	HttpRequest->OnStatusCodeReceived().BindLambda([WeakState, WeakThis](FHttpRequestPtr RequestPtr, int32 StatusCode)
	{
		(void)RequestPtr;
		const TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe> State = WeakState.Pin();
		if (!State.IsValid())
		{
			return;
		}
		bool bScheduleDrain = false;
		{
			const FScopeLock Lock(&State->QueueMutex);
			if (StatusCode >= 200)
			{
				State->HttpStatus = StatusCode;
			}
			if (State->HttpStatus != 0 && !State->PendingChunks.IsEmpty() && !State->bDrainScheduled)
			{
				State->bDrainScheduled = true;
				bScheduleDrain = true;
			}
		}

		if (bScheduleDrain)
		{
			AsyncTask(ENamedThreads::GameThread, [WeakThis, RequestId = State->RequestId]()
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
		State->RequestId);

	ActiveStreamRequests.Add(State->RequestId, State);
	if (!HttpRequest->ProcessRequest())
	{
		ActiveStreamRequests.Remove(State->RequestId);
		FailStateBeforeStart(UnrealAIClientPrivate::MakeStreamError(
			TEXT("Failed to start streaming HTTP request."),
			TEXT("stream_start_failed")));
		return FUnrealAIRequestHandle();
	}

	FUnrealAIRequestHandle Handle;
	Handle.Id = State->RequestId;
	return Handle;
}

bool UUnrealAIClient::CancelRequest(const FUnrealAIRequestHandle& RequestHandle)
{
	if (!RequestHandle.IsValid())
	{
		return false;
	}

	TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>* StatePtr = ActiveStreamRequests.Find(RequestHandle.Id);
	if (!StatePtr)
	{
		return false;
	}

	DrainStreamRequest(RequestHandle.Id);
	StatePtr = ActiveStreamRequests.Find(RequestHandle.Id);
	if (!StatePtr)
	{
		return false;
	}

	const TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe> State = *StatePtr;
	const FHttpRequestPtr HttpRequest = State->HttpRequest;
	FUnrealAIError NoError;
	CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Cancelled, NoError);
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
	EUnrealAIProviderApi ProviderApi,
	FString ResolvedModel,
	FUnrealAIChatCompletionNativeDelegate CompletionDelegate)
{
	InFlightRequests.Remove(HttpRequest);

	FUnrealAIChatResponse ParsedResponse;
	FUnrealAIError Error;
	if (!bWasSuccessful || !HttpResponse.IsValid())
	{
		Error = UnrealAIProviderAdapters::MakeError(
			TEXT("HTTP request failed before a provider response was received."));
		CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
		return;
	}

	const IUnrealAIProviderAdapter& Adapter = UnrealAIProviderAdapters::Get(ProviderApi);
	Adapter.ParseResponse(
		ResolvedModel,
		HttpResponse->GetResponseCode(),
		HttpResponse->GetContentAsString(),
		ParsedResponse,
		Error);
	CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
}

void UUnrealAIClient::DrainStreamRequest(const FGuid& RequestId)
{
	TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>* StatePtr = ActiveStreamRequests.Find(RequestId);
	if (!StatePtr || (*StatePtr)->bTerminal)
	{
		return;
	}

	const TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe> State = *StatePtr;
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
				if (!ActiveStreamRequests.Contains(RequestId))
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
	FGuid RequestId)
{
	(void)HttpRequest;
	TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>* StatePtr = ActiveStreamRequests.Find(RequestId);
	if (!StatePtr)
	{
		return;
	}

	if (HttpResponse.IsValid())
	{
		const FScopeLock Lock(&(*StatePtr)->QueueMutex);
		(*StatePtr)->HttpStatus = HttpResponse->GetResponseCode();
	}

	DrainStreamRequest(RequestId);
	StatePtr = ActiveStreamRequests.Find(RequestId);
	if (!StatePtr)
	{
		return;
	}

	const TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe> State = *StatePtr;
	const int32 HttpStatus = HttpResponse.IsValid() ? HttpResponse->GetResponseCode() : State->HttpStatus;
	if (!bWasSuccessful || !HttpResponse.IsValid())
	{
		CompleteStreamRequest(
			State,
			EUnrealAIChatStreamStatus::Failed,
			UnrealAIClientPrivate::MakeStreamError(
				TEXT("Streaming HTTP request ended before a complete provider response was received."),
				TEXT("stream_transport_failed"),
				HttpStatus));
		return;
	}

	if (HttpStatus < 200 || HttpStatus >= 300)
	{
		FUnrealAIChatResponse IgnoredResponse;
		FUnrealAIError ProviderError;
		UnrealAIProviderAdapters::Get(State->ProviderApi).ParseResponse(
			State->ResolvedModel,
			HttpStatus,
			UnrealAIClientPrivate::Utf8BytesToString(State->ErrorBodyBytes),
			IgnoredResponse,
			ProviderError);
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
			if (!ActiveStreamRequests.Contains(RequestId))
			{
				return;
			}
		}
	}

	if (!UnrealAIProviderAdapters::Get(State->ProviderApi).CanCompleteStream(State->ProviderState))
	{
		CompleteStreamRequest(
			State,
			EUnrealAIChatStreamStatus::Failed,
			UnrealAIClientPrivate::MakeStreamError(
				TEXT("The provider stream ended without its required terminal event."),
				TEXT("truncated_stream"),
				HttpStatus));
		return;
	}

	FUnrealAIError NoError;
	CompleteStreamRequest(State, EUnrealAIChatStreamStatus::Completed, NoError);
}

void UUnrealAIClient::CompleteStreamRequest(
	const TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>& State,
	EUnrealAIChatStreamStatus Status,
	const FUnrealAIError& Error)
{
	if (!State.IsValid() || State->bTerminal)
	{
		return;
	}

	State->bTerminal = true;
	{
		const FScopeLock Lock(&State->QueueMutex);
		State->bAcceptData = false;
		State->PendingChunks.Reset();
		State->bDrainScheduled = false;
	}
	ActiveStreamRequests.Remove(State->RequestId);
	if (State->HttpRequest.IsValid())
	{
		State->HttpRequest->OnStatusCodeReceived().Unbind();
		State->HttpRequest->OnProcessRequestComplete().Unbind();
		State->HttpRequest.Reset();
	}

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
}

void UUnrealAIClient::CancelAllStreams()
{
	TArray<TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>> States;
	ActiveStreamRequests.GenerateValueArray(States);
	ActiveStreamRequests.Reset();
	for (const TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>& State : States)
	{
		if (!State.IsValid())
		{
			continue;
		}
		{
			const FScopeLock Lock(&State->QueueMutex);
			State->bAcceptData = false;
			State->PendingChunks.Reset();
		}
		State->bTerminal = true;
		State->EventDelegate.Unbind();
		State->TerminalDelegate.Unbind();
		const FHttpRequestPtr HttpRequest = State->HttpRequest;
		if (HttpRequest.IsValid())
		{
			HttpRequest->OnStatusCodeReceived().Unbind();
			HttpRequest->OnProcessRequestComplete().Unbind();
			State->HttpRequest.Reset();
			HttpRequest->CancelRequest();
		}
	}
}

void UUnrealAIClient::BeginDestroy()
{
	CancelAllStreams();
	for (const FHttpRequestPtr& Request : InFlightRequests)
	{
		if (Request.IsValid())
		{
			Request->CancelRequest();
		}
	}
	InFlightRequests.Reset();
	Super::BeginDestroy();
}
