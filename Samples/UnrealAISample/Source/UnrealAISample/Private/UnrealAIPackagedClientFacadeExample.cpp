#include "UnrealAIPackagedClientFacadeExample.h"

#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIClient.h"
#include "UnrealAIProductionDeploymentExample.h"
#include "UnrealAISampleStringBounds.h"

namespace UnrealAIPackagedClientFacadeExample
{
	constexpr int32 MaxMessages = 17;
	constexpr int32 MaxMessageCharacters = 2048;
	constexpr int32 MaxTotalCharacters = 8192;
	// A successful assistant response becomes a message on the next turn, so the
	// output and per-message history limits must stay identical.
	constexpr int32 MaxOutputCharacters = MaxMessageCharacters;
}

bool UUnrealAIPackagedClientFacadeExample::Initialize(
	const FString& ShortLivedGameSessionToken,
	FUnrealAIError& OutError)
{
	OutError = FUnrealAIError();
	if (bRequestInFlight)
	{
		OutError.bIsError = true;
		OutError.Type = TEXT("invalid_request_error");
		OutError.Code = TEXT("request_in_progress");
		OutError.Message = TEXT("Cancel the active backend request before reinitializing the facade.");
		return false;
	}

	if (ShortLivedGameSessionToken.TrimStartAndEnd().IsEmpty())
	{
		OutError.bIsError = true;
		OutError.Type = TEXT("authentication_error");
		OutError.Code = TEXT("missing_game_session");
		OutError.Message = TEXT("A short-lived game-session token is required.");
		return false;
	}

	BackendClient = FUnrealAIProductionDeploymentExample::CreatePackagedClientProxy(
		this,
		ShortLivedGameSessionToken,
		OutError);
	return BackendClient != nullptr;
}

bool UUnrealAIPackagedClientFacadeExample::StreamTurn(
	FGuid RequestId,
	const TArray<FUnrealAIChatMessage>& Messages,
	FName& OutFailureReason)
{
	OutFailureReason = NAME_None;
	if (!BackendClient)
	{
		OutFailureReason = TEXT("not_initialized");
		return false;
	}
	if (bRequestInFlight)
	{
		OutFailureReason = TEXT("chat_busy");
		return false;
	}
	if (!RequestId.IsValid())
	{
		OutFailureReason = TEXT("invalid_request_id");
		return false;
	}
	if (!ValidateMessages(Messages, OutFailureReason))
	{
		return false;
	}

	const FUnrealAIChatRequest Request = BuildBackendRequest(RequestId, Messages);

	bRequestInFlight = true;
	ActiveRequestId = RequestId;
	PartialText.Reset();
	bOutputLimitExceeded = false;
	const FUnrealAIRequestHandle StartedRequest = BackendClient->StreamChatCompletion(
		Request,
		FUnrealAIChatStreamEventNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleStreamEvent),
		FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleTerminal),
		FUnrealAIRetryNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleRetry));

	// A configuration or request-building failure can terminate synchronously.
	if (bRequestInFlight)
	{
		ActiveRequest = StartedRequest;
		if (!ActiveRequest.IsValid())
		{
			const FGuid FailedRequestId = ActiveRequestId;
			ClearActiveRequest();
			if (!bShuttingDown)
			{
				OnFailed.Broadcast(FailedRequestId, TEXT("request_not_started"), FString());
			}
		}
	}
	return true;
}

FUnrealAIChatRequest UUnrealAIPackagedClientFacadeExample::BuildBackendRequest(
	const FGuid& RequestId,
	const TArray<FUnrealAIChatMessage>& Messages)
{
	FUnrealAIChatRequest Request;
	Request.Messages = Messages;
	for (FUnrealAIChatMessage& Message : Request.Messages)
	{
		Message.Content = Message.Content.TrimStartAndEnd();
	}
	Request.NumChoices = 1;
	Request.bUseMaxCompletionTokens = true;
	Request.MaxCompletionTokens = 512;
	// This backend-owned extension is stable for every transport attempt made by
	// the SDK. The compatible backend must consume it for idempotency and strip it
	// before forwarding the provider request.
	Request.AdditionalParametersJson = FString::Printf(
		TEXT("{\"game_request_id\":\"%s\"}"),
		*RequestId.ToString(EGuidFormats::DigitsWithHyphensLower));
	return Request;
}

bool UUnrealAIPackagedClientFacadeExample::CancelActiveTurn()
{
	return BackendClient && ActiveRequest.IsValid()
		&& BackendClient->CancelRequest(ActiveRequest);
}

void UUnrealAIPackagedClientFacadeExample::BeginDestroy()
{
	bShuttingDown = true;
	if (BackendClient && ActiveRequest.IsValid())
	{
		BackendClient->CancelRequest(ActiveRequest);
	}
	ClearActiveRequest();
	Super::BeginDestroy();
}

bool UUnrealAIPackagedClientFacadeExample::ValidateMessages(
	const TArray<FUnrealAIChatMessage>& Messages,
	FName& OutFailureReason)
{
	using namespace UnrealAIPackagedClientFacadeExample;

	if (Messages.IsEmpty() || Messages.Num() > MaxMessages)
	{
		OutFailureReason = TEXT("invalid_message_count");
		return false;
	}

	int32 TotalCharacters = 0;
	for (int32 Index = 0; Index < Messages.Num(); ++Index)
	{
		const FUnrealAIChatMessage& Message = Messages[Index];
		if (!Message.ContentJson.IsEmpty()
			|| !Message.Name.IsEmpty()
			|| !Message.ToolCallId.IsEmpty()
			|| !Message.AdditionalFieldsJson.IsEmpty())
		{
			OutFailureReason = TEXT("unsupported_message_fields");
			return false;
		}
		const EUnrealAIMessageRole ExpectedRole = Index % 2 == 0
			? EUnrealAIMessageRole::User
			: EUnrealAIMessageRole::Assistant;
		if (Message.Role != ExpectedRole)
		{
			OutFailureReason = TEXT("invalid_role_order");
			return false;
		}

		const FString NormalizedContent = Message.Content.TrimStartAndEnd();
		if (NormalizedContent.IsEmpty() || NormalizedContent.Len() > MaxMessageCharacters)
		{
			OutFailureReason = TEXT("invalid_message_content");
			return false;
		}
		TotalCharacters += NormalizedContent.Len();
		if (TotalCharacters > MaxTotalCharacters)
		{
			OutFailureReason = TEXT("conversation_too_large");
			return false;
		}
	}

	if (Messages.Last().Role != EUnrealAIMessageRole::User)
	{
		OutFailureReason = TEXT("last_message_must_be_user");
		return false;
	}
	return true;
}

FName UUnrealAIPackagedClientFacadeExample::MapFailureReason(
	const FUnrealAIChatStreamResult& Result)
{
	if (Result.Error.HttpStatus == 401)
	{
		return TEXT("session_expired");
	}
	if (Result.Error.HttpStatus == 403)
	{
		return TEXT("access_denied");
	}
	if (Result.Error.HttpStatus == 429)
	{
		return TEXT("rate_limited");
	}
	if (Result.Error.Code == TEXT("empty_response"))
	{
		return TEXT("empty_response");
	}
	return TEXT("service_unavailable");
}

void UUnrealAIPackagedClientFacadeExample::ClearActiveRequest()
{
	ActiveRequest = FUnrealAIRequestHandle();
	ActiveRequestId.Invalidate();
	bRequestInFlight = false;
}

void UUnrealAIPackagedClientFacadeExample::HandleStreamEvent(
	const FUnrealAIChatStreamEvent& Event)
{
	if (!bRequestInFlight || Event.Type != EUnrealAIChatStreamEventType::TextDelta)
	{
		return;
	}

	using namespace UnrealAIPackagedClientFacadeExample;
	const int32 RemainingCharacters = FMath::Max(0, MaxOutputCharacters - PartialText.Len());
	const FString BoundedDelta = UnrealAISampleStringBounds::LeftAtCodePointBoundary(
		Event.TextDelta,
		RemainingCharacters);
	PartialText += BoundedDelta;
	if (!bShuttingDown && !BoundedDelta.IsEmpty())
	{
		OnTextDelta.Broadcast(ActiveRequestId, BoundedDelta);
	}
	if (BoundedDelta.Len() != Event.TextDelta.Len())
	{
		bOutputLimitExceeded = true;
		if (BackendClient && ActiveRequest.IsValid())
		{
			BackendClient->CancelRequest(ActiveRequest);
		}
	}
}

void UUnrealAIPackagedClientFacadeExample::HandleRetry(
	const FUnrealAIRetryEvent& RetryEvent)
{
	if (bRequestInFlight && !bShuttingDown)
	{
		OnRetrying.Broadcast(
			ActiveRequestId,
			RetryEvent.RetryNumber,
			RetryEvent.MaxRetries,
			RetryEvent.DelaySeconds);
	}
}

void UUnrealAIPackagedClientFacadeExample::HandleTerminal(
	const FUnrealAIChatStreamResult& Result)
{
	if (!bRequestInFlight)
	{
		return;
	}

	const FGuid CompletedRequestId = ActiveRequestId;
	FString TerminalText = PartialText;
	bool bHasAggregateText = false;
	FString AggregateText = UUnrealAIBlueprintLibrary::GetFirstChoiceContent(
		Result.Response,
		bHasAggregateText);
	if (bHasAggregateText)
	{
		AggregateText = AggregateText.TrimStartAndEnd();
		bHasAggregateText = !AggregateText.IsEmpty();
		if (AggregateText.Len() > UnrealAIPackagedClientFacadeExample::MaxOutputCharacters)
		{
			bOutputLimitExceeded = true;
		}
		TerminalText = UnrealAISampleStringBounds::LeftAtCodePointBoundary(
			AggregateText,
			UnrealAIPackagedClientFacadeExample::MaxOutputCharacters);
	}
	const bool bExceededOutputLimit = bOutputLimitExceeded;
	ClearActiveRequest();
	PartialText.Reset();
	bOutputLimitExceeded = false;
	if (bShuttingDown)
	{
		return;
	}

#if WITH_DEV_AUTOMATION_TESTS
	++TerminalBroadcastCountForTests;
	LastTerminalFailureReasonForTests = NAME_None;
#endif

	if (bExceededOutputLimit)
	{
#if WITH_DEV_AUTOMATION_TESTS
		LastTerminalFailureReasonForTests = TEXT("response_too_large");
#endif
		OnFailed.Broadcast(CompletedRequestId, TEXT("response_too_large"), TerminalText);
		return;
	}
	if (Result.Status == EUnrealAIChatStreamStatus::Completed && bHasAggregateText)
	{
		OnCompleted.Broadcast(CompletedRequestId, TerminalText);
		return;
	}
	if (Result.Status == EUnrealAIChatStreamStatus::Cancelled)
	{
		OnCancelled.Broadcast(CompletedRequestId, TerminalText);
		return;
	}

	FUnrealAIChatStreamResult FailureResult = Result;
	if (Result.Status == EUnrealAIChatStreamStatus::Completed)
	{
		FailureResult.Error.Code = TEXT("empty_response");
	}
	const FName PublicFailureReason = MapFailureReason(FailureResult);
#if WITH_DEV_AUTOMATION_TESTS
	LastTerminalFailureReasonForTests = PublicFailureReason;
#endif
	OnFailed.Broadcast(CompletedRequestId, PublicFailureReason, TerminalText);
}
