#include "UnrealAIChatCompletionAsyncAction.h"

UUnrealAIChatCompletionAsyncAction* UUnrealAIChatCompletionAsyncAction::CreateChatCompletion(UObject* WorldContextObject, FName ProviderName, const FUnrealAIChatRequest& Request)
{
	UUnrealAIChatCompletionAsyncAction* Action = NewObject<UUnrealAIChatCompletionAsyncAction>();
	Action->WorldContext = WorldContextObject;
	Action->Provider = ProviderName;
	Action->PendingRequest = Request;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UUnrealAIChatCompletionAsyncAction::Activate()
{
	Client = NewObject<UUnrealAIClient>(this);

	FUnrealAIError ConfigError;
	if (!Client->ConfigureFromSettings(Provider, ConfigError))
	{
		BroadcastFailure(ConfigError);
		return;
	}

	Client->CreateChatCompletion(PendingRequest, FUnrealAIChatCompletionNativeDelegate::CreateUObject(this, &UUnrealAIChatCompletionAsyncAction::HandleCompletion));
}

void UUnrealAIChatCompletionAsyncAction::HandleCompletion(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error)
{
	if (Error.bIsError)
	{
		Failed.Broadcast(Response, Error);
	}
	else
	{
		Completed.Broadcast(Response, Error);
	}

	SetReadyToDestroy();
}

void UUnrealAIChatCompletionAsyncAction::BroadcastFailure(const FUnrealAIError& Error)
{
	FUnrealAIChatResponse EmptyResponse;
	Failed.Broadcast(EmptyResponse, Error);
	SetReadyToDestroy();
}
