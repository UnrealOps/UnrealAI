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

	Client->CreateChatCompletion(Request, FUnrealAIChatCompletionNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleCompletion));
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
	ActiveStreamHandle = Client->StreamChatCompletion(
		Request,
		FUnrealAIChatStreamEventNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleStreamEvent),
		FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleStreamTerminal));
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

void UUnrealAIChatComponent::HandleCompletion(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error)
{
	if (Error.bIsError)
	{
		OnChatFailed.Broadcast(Response, Error);
	}
	else
	{
		OnChatCompleted.Broadcast(Response, Error);
	}
}

void UUnrealAIChatComponent::HandleStreamEvent(const FUnrealAIChatStreamEvent& Event)
{
	if (!bEndingPlay)
	{
		OnChatStreamEvent.Broadcast(Event);
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
	if (Client && ActiveStreamHandle.IsValid())
	{
		Client->CancelRequest(ActiveStreamHandle);
		ActiveStreamHandle = FUnrealAIRequestHandle();
	}
	Super::EndPlay(EndPlayReason);
}
