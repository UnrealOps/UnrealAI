#include "UnrealAIResponseAsyncAction.h"

UUnrealAIResponseAsyncAction* UUnrealAIResponseAsyncAction::CreateResponse(UObject* WorldContextObject,
	FName ProviderName, const FUnrealAIResponseRequest& InRequest)
{
	UUnrealAIResponseAsyncAction* Action = NewObject<UUnrealAIResponseAsyncAction>();
	Action->WorldContext = WorldContextObject;
	Action->Provider = ProviderName;
	Action->Request = InRequest;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

UUnrealAIResponseAsyncAction* UUnrealAIResponseAsyncAction::StreamResponse(UObject* WorldContextObject,
	FName ProviderName, const FUnrealAIResponseRequest& InRequest)
{
	UUnrealAIResponseAsyncAction* Action = CreateResponse(WorldContextObject, ProviderName, InRequest);
	Action->bStreaming = true;
	return Action;
}

void UUnrealAIResponseAsyncAction::Activate()
{
	if (bActivated || bTerminal)
	{
		return;
	}
	bActivated = true;
	Client = NewObject<UUnrealAIClient>(this);
	FUnrealAIResponseResult Failure;
	if (!Client->ConfigureFromSettings(Provider, Failure.Error))
	{
		HandleTerminal(Failure);
		return;
	}
	FUnrealAIResponseNativeDelegate Terminal = FUnrealAIResponseNativeDelegate::CreateUObject(this, &UUnrealAIResponseAsyncAction::HandleTerminal);
	FUnrealAIRetryNativeDelegate Retry = FUnrealAIRetryNativeDelegate::CreateUObject(this, &UUnrealAIResponseAsyncAction::HandleRetry);
	Handle = bStreaming ? Client->StreamResponse(Request,
		FUnrealAIResponseEventNativeDelegate::CreateUObject(this, &UUnrealAIResponseAsyncAction::HandleEvent),
		Terminal, Retry) : Client->CreateResponse(Request, Terminal, Retry);
}

void UUnrealAIResponseAsyncAction::Cancel()
{
	if (bTerminal)
	{
		return;
	}
	if (Client && Handle.IsValid() && Client->CancelRequest(Handle))
	{
		return;
	}
	FUnrealAIResponseResult Result;
	Result.RequestHandle = Handle;
	Result.Status = EUnrealAIResponseStatus::Cancelled;
	HandleTerminal(Result);
}

void UUnrealAIResponseAsyncAction::HandleTerminal(const FUnrealAIResponseResult& Result)
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
		case EUnrealAIResponseStatus::Completed:
			Completed.Broadcast(Result, FUnrealAIResponseEvent(), FUnrealAIRetryEvent());
			break;
		case EUnrealAIResponseStatus::Incomplete:
			Incomplete.Broadcast(Result, FUnrealAIResponseEvent(), FUnrealAIRetryEvent());
			break;
		case EUnrealAIResponseStatus::Cancelled:
			Cancelled.Broadcast(Result, FUnrealAIResponseEvent(), FUnrealAIRetryEvent());
			break;
		default:
			Failed.Broadcast(Result, FUnrealAIResponseEvent(), FUnrealAIRetryEvent());
			break;
		}
	}
	Handle = FUnrealAIRequestHandle();
	Super::Cancel();
}

void UUnrealAIResponseAsyncAction::HandleEvent(const FUnrealAIResponseEvent& ResponseEvent)
{
	if (!bTerminal && ShouldBroadcastDelegates())
	{
		Event.Broadcast(FUnrealAIResponseResult(), ResponseEvent, FUnrealAIRetryEvent());
	}
}

void UUnrealAIResponseAsyncAction::HandleRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	if (!bTerminal && ShouldBroadcastDelegates())
	{
		Retrying.Broadcast(FUnrealAIResponseResult(), FUnrealAIResponseEvent(), RetryEvent);
	}
}
