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

	RequestHandle = Client->CreateChatCompletion(
		PendingRequest,
		FUnrealAIChatCompletionNativeDelegate::CreateUObject(this, &UUnrealAIChatCompletionAsyncAction::HandleCompletion),
		FUnrealAIRetryNativeDelegate::CreateUObject(this, &UUnrealAIChatCompletionAsyncAction::HandleRetry));
}

void UUnrealAIChatCompletionAsyncAction::Cancel()
{
	if (bTerminal)
	{
		Super::Cancel();
		return;
	}

	if (Client && RequestHandle.IsValid() && Client->CancelRequest(RequestHandle))
	{
		return;
	}

	bTerminal = true;
	FUnrealAIChatResponse EmptyResponse;
	FUnrealAIError Error;
	Error.bIsError = true;
	Error.Type = TEXT("request_cancelled");
	Error.Code = TEXT("request_cancelled");
	Error.Message = TEXT("The UnrealAI request was cancelled.");
	if (ShouldBroadcastDelegates())
	{
		Cancelled.Broadcast(EmptyResponse, Error);
	}
	Super::Cancel();
}

void UUnrealAIChatCompletionAsyncAction::HandleCompletion(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error)
{
	if (bTerminal)
	{
		return;
	}
	bTerminal = true;

	if (ShouldBroadcastDelegates() && Error.Code == TEXT("request_cancelled"))
	{
		Cancelled.Broadcast(Response, Error);
	}
	else if (ShouldBroadcastDelegates() && Error.bIsError)
	{
		Failed.Broadcast(Response, Error);
	}
	else if (ShouldBroadcastDelegates())
	{
		Completed.Broadcast(Response, Error);
	}

	Super::Cancel();
}

void UUnrealAIChatCompletionAsyncAction::HandleRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	if (!bTerminal && ShouldBroadcastDelegates())
	{
		Retrying.Broadcast(RetryEvent);
	}
}

void UUnrealAIChatCompletionAsyncAction::BroadcastFailure(const FUnrealAIError& Error)
{
	if (bTerminal)
	{
		return;
	}
	bTerminal = true;
	FUnrealAIChatResponse EmptyResponse;
	if (ShouldBroadcastDelegates())
	{
		Failed.Broadcast(EmptyResponse, Error);
	}
	Super::Cancel();
}
