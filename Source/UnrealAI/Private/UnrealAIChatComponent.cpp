#include "UnrealAIChatComponent.h"

UUnrealAIChatComponent::UUnrealAIChatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UUnrealAIChatComponent::SendPrompt(const FString& Prompt)
{
	TArray<FUnrealAIChatMessage> Messages;

	if (!SystemPrompt.IsEmpty())
	{
		FUnrealAIChatMessage SystemMessage;
		SystemMessage.Role = EUnrealAIMessageRole::System;
		SystemMessage.Content = SystemPrompt;
		Messages.Add(SystemMessage);
	}

	FUnrealAIChatMessage UserMessage;
	UserMessage.Role = EUnrealAIMessageRole::User;
	UserMessage.Content = Prompt;
	Messages.Add(UserMessage);

	SendMessages(Messages);
}

void UUnrealAIChatComponent::SendMessages(const TArray<FUnrealAIChatMessage>& Messages)
{
	FUnrealAIError ConfigError;
	if (!EnsureClient(ConfigError))
	{
		FUnrealAIChatResponse EmptyResponse;
		OnChatFailed.Broadcast(EmptyResponse, ConfigError);
		return;
	}

	FUnrealAIChatRequest Request;
	Request.Model = Model;
	Request.Messages = Messages;
	Request.bUseTemperature = bUseTemperature;
	Request.Temperature = Temperature;
	Request.RetryOptions = RetryOptions;

	const FGuid CompletionId = FGuid::NewGuid();
	const FUnrealAIRequestHandle RequestHandle = Client->CreateChatCompletion(
		Request,
		FUnrealAIChatCompletionNativeDelegate::CreateWeakLambda(
			this,
			[this, CompletionId](const FUnrealAIChatResponse& Response, const FUnrealAIError& Error)
			{
				HandleCompletion(CompletionId, Response, Error);
			}),
		FUnrealAIRetryNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleRetry));
	if (RequestHandle.IsValid())
	{
		ActiveCompletionHandles.Add(CompletionId, RequestHandle);
	}
}

int32 UUnrealAIChatComponent::CancelActiveCompletions()
{
	if (!Client || ActiveCompletionHandles.IsEmpty())
	{
		return 0;
	}

	TArray<FUnrealAIRequestHandle> Handles;
	ActiveCompletionHandles.GenerateValueArray(Handles);
	int32 CancelledCount = 0;
	for (const FUnrealAIRequestHandle& Handle : Handles)
	{
		if (Client->CancelRequest(Handle))
		{
			++CancelledCount;
		}
	}
	return CancelledCount;
}

void UUnrealAIChatComponent::SendPromptStream(const FString& Prompt)
{
	TArray<FUnrealAIChatMessage> Messages;
	if (!SystemPrompt.IsEmpty())
	{
		FUnrealAIChatMessage SystemMessage;
		SystemMessage.Role = EUnrealAIMessageRole::System;
		SystemMessage.Content = SystemPrompt;
		Messages.Add(SystemMessage);
	}

	FUnrealAIChatMessage UserMessage;
	UserMessage.Role = EUnrealAIMessageRole::User;
	UserMessage.Content = Prompt;
	Messages.Add(UserMessage);
	SendMessagesStream(Messages);
}

void UUnrealAIChatComponent::SendMessagesStream(const TArray<FUnrealAIChatMessage>& Messages)
{
	if (ActiveStreamHandle.IsValid())
	{
		FUnrealAIChatResponse EmptyResponse;
		FUnrealAIError Error;
		Error.bIsError = true;
		Error.Type = TEXT("stream_error");
		Error.Code = TEXT("stream_already_active");
		Error.Message = TEXT("This UnrealAIChatComponent already has an active stream.");
		OnChatStreamFailed.Broadcast(EmptyResponse, Error);
		return;
	}

	FUnrealAIError ConfigError;
	if (!EnsureClient(ConfigError))
	{
		FUnrealAIChatResponse EmptyResponse;
		OnChatStreamFailed.Broadcast(EmptyResponse, ConfigError);
		return;
	}

	FUnrealAIChatRequest Request;
	Request.Model = Model;
	Request.Messages = Messages;
	Request.bUseTemperature = bUseTemperature;
	Request.Temperature = Temperature;
	Request.RetryOptions = RetryOptions;
	ActiveStreamHandle = Client->StreamChatCompletion(
		Request,
		FUnrealAIChatStreamEventNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleStreamEvent),
		FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleStreamTerminal),
		FUnrealAIRetryNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleStreamRetry));
}

bool UUnrealAIChatComponent::CancelActiveStream()
{
	return Client && ActiveStreamHandle.IsValid() && Client->CancelRequest(ActiveStreamHandle);
}

bool UUnrealAIChatComponent::EnsureClient(FUnrealAIError& OutError)
{
	if (!Client)
	{
		Client = NewObject<UUnrealAIClient>(this);
	}

	return Client->ConfigureFromSettings(ProviderName, OutError);
}

void UUnrealAIChatComponent::HandleCompletion(
	FGuid CompletionId,
	const FUnrealAIChatResponse& Response,
	const FUnrealAIError& Error)
{
	ActiveCompletionHandles.Remove(CompletionId);
	if (bEndingPlay)
	{
		return;
	}

	if (Error.Code == TEXT("request_cancelled"))
	{
		OnChatCancelled.Broadcast(Response, Error);
	}
	else if (Error.bIsError)
	{
		OnChatFailed.Broadcast(Response, Error);
	}
	else
	{
		OnChatCompleted.Broadcast(Response, Error);
	}
}

void UUnrealAIChatComponent::HandleRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	if (!bEndingPlay)
	{
		OnChatRetrying.Broadcast(RetryEvent);
	}
}

void UUnrealAIChatComponent::HandleStreamEvent(const FUnrealAIChatStreamEvent& Event)
{
	if (!bEndingPlay)
	{
		OnChatStreamEvent.Broadcast(Event);
	}
}

void UUnrealAIChatComponent::HandleStreamRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	if (!bEndingPlay)
	{
		OnChatStreamRetrying.Broadcast(RetryEvent);
	}
}

void UUnrealAIChatComponent::HandleStreamTerminal(const FUnrealAIChatStreamResult& Result)
{
	ActiveStreamHandle = FUnrealAIRequestHandle();
	if (bEndingPlay)
	{
		return;
	}

	switch (Result.Status)
	{
	case EUnrealAIChatStreamStatus::Completed:
		OnChatStreamCompleted.Broadcast(Result.Response, Result.Error);
		break;
	case EUnrealAIChatStreamStatus::Cancelled:
		OnChatStreamCancelled.Broadcast(Result.Response);
		break;
	case EUnrealAIChatStreamStatus::Failed:
	default:
		OnChatStreamFailed.Broadcast(Result.Response, Result.Error);
		break;
	}
}

void UUnrealAIChatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	CancelActiveCompletions();
	ActiveCompletionHandles.Reset();
	if (Client && ActiveStreamHandle.IsValid())
	{
		Client->CancelRequest(ActiveStreamHandle);
		ActiveStreamHandle = FUnrealAIRequestHandle();
	}
	Super::EndPlay(EndPlayReason);
}
