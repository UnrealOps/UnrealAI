#include "UnrealAIChatStreamAsyncAction.h"

UUnrealAIChatStreamAsyncAction* UUnrealAIChatStreamAsyncAction::StreamChatCompletion(
	UObject* WorldContextObject,
	FName ProviderName,
	const FUnrealAIChatRequest& Request)
{
	UUnrealAIChatStreamAsyncAction* Action = NewObject<UUnrealAIChatStreamAsyncAction>();
	Action->WorldContext = WorldContextObject;
	Action->Provider = ProviderName;
	Action->PendingRequest = Request;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UUnrealAIChatStreamAsyncAction::Activate()
{
	Client = NewObject<UUnrealAIClient>(this);

	FUnrealAIError ConfigError;
	if (!Client->ConfigureFromSettings(Provider, ConfigError))
	{
		BroadcastFailure(ConfigError);
		return;
	}

	RequestHandle = Client->StreamChatCompletion(
		PendingRequest,
		FUnrealAIChatStreamEventNativeDelegate::CreateUObject(this, &UUnrealAIChatStreamAsyncAction::HandleEvent),
		FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(this, &UUnrealAIChatStreamAsyncAction::HandleTerminal));
}

void UUnrealAIChatStreamAsyncAction::Cancel()
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
	if (ShouldBroadcastDelegates())
	{
		Cancelled.Broadcast(EmptyResponse);
	}
	Super::Cancel();
}

void UUnrealAIChatStreamAsyncAction::HandleEvent(const FUnrealAIChatStreamEvent& StreamEvent)
{
	if (!bTerminal && ShouldBroadcastDelegates())
	{
		Event.Broadcast(StreamEvent);
	}
}

void UUnrealAIChatStreamAsyncAction::HandleTerminal(const FUnrealAIChatStreamResult& Result)
{
	if (bTerminal)
	{
		return;
	}
	bTerminal = true;

	if (ShouldBroadcastDelegates())
	{
		switch (Result.Status)
		{
		case EUnrealAIChatStreamStatus::Completed:
			Completed.Broadcast(Result.Response, Result.Error);
			break;
		case EUnrealAIChatStreamStatus::Cancelled:
			Cancelled.Broadcast(Result.Response);
			break;
		case EUnrealAIChatStreamStatus::Failed:
		default:
			Failed.Broadcast(Result.Response, Result.Error);
			break;
		}
	}

	Super::Cancel();
}

void UUnrealAIChatStreamAsyncAction::BroadcastFailure(const FUnrealAIError& Error)
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
