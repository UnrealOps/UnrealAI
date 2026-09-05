#include "UnrealAIMultiTurnExample.h"

#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIClient.h"

bool AUnrealAIMultiTurnExample::ResetConversation()
{
	if (bRequestInFlight)
	{
		return false;
	}

	ConversationHistory.Reset();
	PendingUserMessage = FUnrealAIChatMessage();
	if (!SystemPrompt.TrimStartAndEnd().IsEmpty())
	{
		ConversationHistory.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
			EUnrealAIMessageRole::System,
			SystemPrompt));
	}
	return true;
}

bool AUnrealAIMultiTurnExample::SendTurn(const FString& UserText, FUnrealAIError& OutError)
{
	FUnrealAIChatRequest Request;
	if (!PrepareTurn(UserText, Request, OutError))
	{
		return false;
	}
	bStreamingTurn = false;

	const FUnrealAIRequestHandle StartedRequest = UnrealAIClient->CreateChatCompletion(
		Request,
		FUnrealAIChatCompletionNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleTurnCompleted),
		FUnrealAIRetryNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleTurnRetry));

	// Configuration and request-building failures invoke the callback synchronously.
	if (bRequestInFlight)
	{
		ActiveRequest = StartedRequest;
		if (!ActiveRequest.IsValid())
		{
			bRequestInFlight = false;
			RollBackPendingUserTurn();
			BroadcastLocalError(TEXT("request_not_started"), TEXT("UnrealAI did not start the request."));
		}
	}
	return true;
}

bool AUnrealAIMultiTurnExample::SendTurnStream(const FString& UserText, FUnrealAIError& OutError)
{
	FUnrealAIChatRequest Request;
	if (!PrepareTurn(UserText, Request, OutError))
	{
		return false;
	}
	bStreamingTurn = true;

	const FUnrealAIRequestHandle StartedRequest = UnrealAIClient->StreamChatCompletion(
		Request,
		FUnrealAIChatStreamEventNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleStreamEvent),
		FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleStreamTerminal),
		FUnrealAIRetryNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleTurnRetry));

	// Configuration and request-building failures can invoke the terminal synchronously.
	if (bRequestInFlight)
	{
		ActiveRequest = StartedRequest;
		if (!ActiveRequest.IsValid())
		{
			bRequestInFlight = false;
			bStreamingTurn = false;
			RollBackPendingUserTurn();
			BroadcastLocalError(TEXT("request_not_started"), TEXT("UnrealAI did not start the stream."));
		}
	}
	return true;
}

bool AUnrealAIMultiTurnExample::CancelTurn()
{
	return UnrealAIClient && ActiveRequest.IsValid()
		&& UnrealAIClient->CancelRequest(ActiveRequest);
}

void AUnrealAIMultiTurnExample::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	if (UnrealAIClient && ActiveRequest.IsValid())
	{
		UnrealAIClient->CancelRequest(ActiveRequest);
	}
	ActiveRequest = FUnrealAIRequestHandle();
	bRequestInFlight = false;
	bStreamingTurn = false;
	PendingUserMessage = FUnrealAIChatMessage();
	PendingAssistantText.Reset();
	Super::EndPlay(EndPlayReason);
}

bool AUnrealAIMultiTurnExample::EnsureClient(FUnrealAIError& OutError)
{
	if (!UnrealAIClient)
	{
		UnrealAIClient = NewObject<UUnrealAIClient>(this);
	}
	return UnrealAIClient->ConfigureFromSettings(ProviderName, OutError);
}

bool AUnrealAIMultiTurnExample::PrepareTurn(
	const FString& UserText,
	FUnrealAIChatRequest& OutRequest,
	FUnrealAIError& OutError)
{
	OutError = FUnrealAIError();
	if (bRequestInFlight)
	{
		SetLocalError(
			OutError,
			TEXT("turn_in_progress"),
			TEXT("Wait for the active turn to finish or cancel it before sending another turn."));
		return false;
	}

	const FString NormalizedUserText = UserText.TrimStartAndEnd();
	if (NormalizedUserText.IsEmpty())
	{
		SetLocalError(OutError, TEXT("empty_user_message"), TEXT("The user message cannot be empty."));
		return false;
	}

	if (!EnsureClient(OutError))
	{
		return false;
	}

	if (ConversationHistory.IsEmpty())
	{
		ResetConversation();
	}
	TrimCompletedHistory();

	PendingUserMessage = UUnrealAIBlueprintLibrary::MakeChatMessage(
		EUnrealAIMessageRole::User,
		NormalizedUserText);
	OutRequest = FUnrealAIChatRequest();
	OutRequest.Messages = ConversationHistory;
	OutRequest.Messages.Add(PendingUserMessage);
	bRequestInFlight = true;
	PendingAssistantText.Reset();
	LastInterruptedAssistantText.Reset();
	return true;
}

void AUnrealAIMultiTurnExample::TrimCompletedHistory()
{
	const int32 PrefixCount = ConversationHistory.Num() > 0
		&& ConversationHistory[0].Role == EUnrealAIMessageRole::System
		? 1
		: 0;
	const int32 MaxCompletedMessages = FMath::Max(1, MaxRetainedTurns) * 2;
	while (ConversationHistory.Num() - PrefixCount > MaxCompletedMessages)
	{
		ConversationHistory.RemoveAt(PrefixCount, 2, EAllowShrinking::No);
	}
}

void AUnrealAIMultiTurnExample::RollBackPendingUserTurn()
{
	PendingUserMessage = FUnrealAIChatMessage();
}

bool AUnrealAIMultiTurnExample::CommitAssistantResponse(
	const FUnrealAIChatResponse& Response,
	FString& OutAssistantText)
{
	bool bHasContent = false;
	OutAssistantText = UUnrealAIBlueprintLibrary::GetFirstChoiceContent(Response, bHasContent);
	OutAssistantText = OutAssistantText.TrimStartAndEnd();
	if (!bHasContent || OutAssistantText.IsEmpty())
	{
		return false;
	}

	ConversationHistory.Add(PendingUserMessage);
	ConversationHistory.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
		EUnrealAIMessageRole::Assistant,
		OutAssistantText));
	PendingUserMessage = FUnrealAIChatMessage();
	return true;
}

void AUnrealAIMultiTurnExample::SetLocalError(
	FUnrealAIError& OutError,
	const FString& Code,
	const FString& Message)
{
	OutError.bIsError = true;
	OutError.Type = TEXT("invalid_request_error");
	OutError.Code = Code;
	OutError.Message = Message;
}

void AUnrealAIMultiTurnExample::BroadcastLocalError(const FString& Code, const FString& Message)
{
	if (bEndingPlay)
	{
		return;
	}

	FUnrealAIError Error;
	SetLocalError(Error, Code, Message);
	OnTurnFailed.Broadcast(Error);
}

void AUnrealAIMultiTurnExample::HandleTurnCompleted(
	const FUnrealAIChatResponse& Response,
	const FUnrealAIError& Error)
{
	if (!bRequestInFlight || bStreamingTurn)
	{
		return;
	}

	ActiveRequest = FUnrealAIRequestHandle();
	bRequestInFlight = false;

	if (Error.bIsError)
	{
		RollBackPendingUserTurn();
		if (bEndingPlay)
		{
			return;
		}
		if (Error.Code == TEXT("request_cancelled"))
		{
			OnTurnCancelled.Broadcast();
		}
		else
		{
			OnTurnFailed.Broadcast(Error);
		}
		return;
	}

	FString AssistantText;
	if (!CommitAssistantResponse(Response, AssistantText))
	{
		RollBackPendingUserTurn();
		BroadcastLocalError(TEXT("empty_response"), TEXT("UnrealAI returned no assistant text."));
		return;
	}

	if (!bEndingPlay)
	{
		OnTurnCompleted.Broadcast(AssistantText);
	}
}

void AUnrealAIMultiTurnExample::HandleTurnRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	if (!bEndingPlay && bRequestInFlight)
	{
		OnTurnRetrying.Broadcast(RetryEvent);
	}
}

void AUnrealAIMultiTurnExample::HandleStreamEvent(const FUnrealAIChatStreamEvent& Event)
{
	if (!bRequestInFlight || !bStreamingTurn
		|| Event.Type != EUnrealAIChatStreamEventType::TextDelta)
	{
		return;
	}

	PendingAssistantText += Event.TextDelta;
	if (!bEndingPlay)
	{
		OnTurnTextDelta.Broadcast(Event.TextDelta);
	}
}

void AUnrealAIMultiTurnExample::HandleStreamTerminal(const FUnrealAIChatStreamResult& Result)
{
	if (!bRequestInFlight || !bStreamingTurn)
	{
		return;
	}

	ActiveRequest = FUnrealAIRequestHandle();
	bRequestInFlight = false;
	bStreamingTurn = false;

	FString AssistantText;
	if (Result.Status == EUnrealAIChatStreamStatus::Completed
		&& CommitAssistantResponse(Result.Response, AssistantText))
	{
		PendingAssistantText = AssistantText;
		LastInterruptedAssistantText.Reset();
		if (!bEndingPlay)
		{
			OnTurnCompleted.Broadcast(AssistantText);
		}
		PendingAssistantText.Reset();
		return;
	}

	LastInterruptedAssistantText = PendingAssistantText;
	PendingAssistantText.Reset();
	RollBackPendingUserTurn();
	if (bEndingPlay)
	{
		return;
	}

	if (Result.Status == EUnrealAIChatStreamStatus::Cancelled)
	{
		OnTurnCancelled.Broadcast();
		return;
	}

	if (Result.Status == EUnrealAIChatStreamStatus::Completed)
	{
		BroadcastLocalError(TEXT("empty_response"), TEXT("UnrealAI returned no assistant text."));
		return;
	}

	OnTurnFailed.Broadcast(Result.Error);
}
