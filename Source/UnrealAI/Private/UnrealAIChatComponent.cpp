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
