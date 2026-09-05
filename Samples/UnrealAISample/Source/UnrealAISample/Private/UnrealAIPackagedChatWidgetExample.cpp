#include "UnrealAIPackagedChatWidgetExample.h"

#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIPackagedClientFacadeExample.h"
#include "UnrealAISampleStringBounds.h"

namespace UnrealAIPackagedChatWidgetExample
{
	constexpr int32 MaxCommittedMessages = 16;
	constexpr int32 MaxMessageCharacters = 2048;
	constexpr int32 MaxTotalCharacters = 8192;
}

bool UUnrealAIPackagedChatWidgetExample::AttachInitializedFacade(
	UUnrealAIPackagedClientFacadeExample* InFacade)
{
	if (!InFacade || !InFacade->IsInitialized()
		|| ActiveRequestId.IsValid() || InFacade->bRequestInFlight)
	{
		return false;
	}

	if (Facade)
	{
		Facade->OnTextDelta.RemoveAll(this);
		Facade->OnRetrying.RemoveAll(this);
		Facade->OnCompleted.RemoveAll(this);
		Facade->OnFailed.RemoveAll(this);
		Facade->OnCancelled.RemoveAll(this);
	}

	Facade = InFacade;
	Facade->OnTextDelta.AddDynamic(this, &ThisClass::HandleTextDelta);
	Facade->OnRetrying.AddDynamic(this, &ThisClass::HandleRetrying);
	Facade->OnCompleted.AddDynamic(this, &ThisClass::HandleCompleted);
	Facade->OnFailed.AddDynamic(this, &ThisClass::HandleFailed);
	Facade->OnCancelled.AddDynamic(this, &ThisClass::HandleCancelled);
	bAcceptEvents = true;
	bBackendReady = true;
	if (RequestState != EUnrealAIPackagedChatState::Failed
		&& RequestState != EUnrealAIPackagedChatState::Cancelled)
	{
		RequestState = EUnrealAIPackagedChatState::Idle;
	}
	ReceiveBackendReadinessChanged(true);
	return true;
}

bool UUnrealAIPackagedChatWidgetExample::SubmitTurn(
	const FString& UserText,
	FName& OutFailureReason)
{
	using namespace UnrealAIPackagedChatWidgetExample;
	OutFailureReason = NAME_None;
	const FString NormalizedUserText = UserText.TrimStartAndEnd();
	if (!Facade || !bAcceptEvents)
	{
		OutFailureReason = TEXT("not_initialized");
		return false;
	}
	if (!bBackendReady)
	{
		OutFailureReason = TEXT("backend_refreshing");
		return false;
	}
	if (ActiveRequestId.IsValid() || Facade->bRequestInFlight)
	{
		OutFailureReason = TEXT("chat_busy");
		return false;
	}
	if (NormalizedUserText.IsEmpty() || NormalizedUserText.Len() > MaxMessageCharacters)
	{
		OutFailureReason = TEXT("invalid_message_content");
		return false;
	}

	TArray<FUnrealAIChatMessage> RequestMessages = CommittedHistory;
	TrimHistoryForRequest(RequestMessages, NormalizedUserText);
	if (CountHistoryCharacters(RequestMessages) + NormalizedUserText.Len() > MaxTotalCharacters)
	{
		OutFailureReason = TEXT("conversation_too_large");
		return false;
	}

	RequestMessages.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
		EUnrealAIMessageRole::User,
		NormalizedUserText));
	PendingUserText = NormalizedUserText;
	PendingAssistantText.Reset();
	InterruptedAssistantText.Reset();
	LastPublicFailureReason = NAME_None;
	ActiveRequestId = FGuid::NewGuid();
	RequestState = EUnrealAIPackagedChatState::Starting;

	const FGuid StartedRequestId = ActiveRequestId;
	bool bStreamStarted = false;
#if WITH_DEV_AUTOMATION_TESTS
	if (StreamTurnForTests)
	{
		bStreamStarted = StreamTurnForTests(
			StartedRequestId,
			RequestMessages,
			OutFailureReason);
	}
	else
#endif
	{
		bStreamStarted = Facade->StreamTurn(
			StartedRequestId,
			RequestMessages,
			OutFailureReason);
	}

	if (!bStreamStarted)
	{
		// A transport seam may have delivered a synchronous terminal before
		// returning false. Its terminal state and presentation must win.
		if (!IsActiveEvent(StartedRequestId))
		{
			return false;
		}

		RetryUserText = PendingUserText;
		LastPublicFailureReason = OutFailureReason.IsNone()
			? FName(TEXT("request_not_started"))
			: OutFailureReason;
		ClearActiveState(EUnrealAIPackagedChatState::Failed);
		ReceiveFailed(LastPublicFailureReason, FString());
		return false;
	}
	// A terminal callback may have run synchronously. Do not rewrite state here.
	return true;
}

bool UUnrealAIPackagedChatWidgetExample::RetryLastTurn(FName& OutFailureReason)
{
	if (ActiveRequestId.IsValid() || RetryUserText.IsEmpty()
		|| (RequestState != EUnrealAIPackagedChatState::Failed
			&& RequestState != EUnrealAIPackagedChatState::Cancelled))
	{
		OutFailureReason = TEXT("nothing_to_retry");
		return false;
	}

	const FString UserText = RetryUserText;
	RetryUserText.Reset();
	const bool bStarted = SubmitTurn(UserText, OutFailureReason);
	if (!bStarted && RetryUserText.IsEmpty())
	{
		// A local rejection (for example a facade refresh still in progress) did
		// not start a request. Preserve the user's explicit retry action.
		RetryUserText = UserText;
	}
	return bStarted;
}

bool UUnrealAIPackagedChatWidgetExample::CancelTurn()
{
	if (!Facade || !ActiveRequestId.IsValid())
	{
		return false;
	}

	const EUnrealAIPackagedChatState PreviousState = RequestState;
	const FGuid CancelledRequestId = ActiveRequestId;
	RequestState = EUnrealAIPackagedChatState::Cancelling;
	if (!Facade->CancelActiveTurn())
	{
		ReconcileRejectedCancel(CancelledRequestId, PreviousState);
		return false;
	}
	return true;
}

void UUnrealAIPackagedChatWidgetExample::NativeDestruct()
{
	bAcceptEvents = false;
	bBackendReady = false;
	RequestState = EUnrealAIPackagedChatState::Destroying;
	if (Facade)
	{
		Facade->OnTextDelta.RemoveAll(this);
		Facade->OnRetrying.RemoveAll(this);
		Facade->OnCompleted.RemoveAll(this);
		Facade->OnFailed.RemoveAll(this);
		Facade->OnCancelled.RemoveAll(this);
		Facade->CancelActiveTurn();
	}
	Facade = nullptr;
	ActiveRequestId.Invalidate();
	PendingUserText.Reset();
	PendingAssistantText.Reset();
	Super::NativeDestruct();
}

void UUnrealAIPackagedChatWidgetExample::HandleTextDelta(
	FGuid RequestId,
	const FString& TextDelta)
{
	if (!IsActiveEvent(RequestId))
	{
		return;
	}

	using namespace UnrealAIPackagedChatWidgetExample;
	const FString BoundedDelta = UnrealAISampleStringBounds::LeftAtCodePointBoundary(
		TextDelta,
		FMath::Max(0, MaxMessageCharacters - PendingAssistantText.Len()));
	PendingAssistantText += BoundedDelta;
	RequestState = EUnrealAIPackagedChatState::Streaming;
	if (!BoundedDelta.IsEmpty())
	{
		ReceiveTextDelta(BoundedDelta);
	}
}

void UUnrealAIPackagedChatWidgetExample::HandleRetrying(
	FGuid RequestId,
	int32 RetryNumber,
	int32 MaxRetries,
	float DelaySeconds)
{
	if (IsActiveEvent(RequestId))
	{
		RequestState = EUnrealAIPackagedChatState::Retrying;
		ReceiveRetrying(RetryNumber, MaxRetries, DelaySeconds);
	}
}

void UUnrealAIPackagedChatWidgetExample::HandleCompleted(
	FGuid RequestId,
	const FString& FinalText)
{
	if (!IsActiveEvent(RequestId))
	{
		return;
	}

	using namespace UnrealAIPackagedChatWidgetExample;
	const FString NormalizedFinalText = FinalText.TrimStartAndEnd();
	if (NormalizedFinalText.IsEmpty() || NormalizedFinalText.Len() > MaxMessageCharacters)
	{
		const FName Reason = NormalizedFinalText.IsEmpty()
			? FName(TEXT("empty_response"))
			: FName(TEXT("response_too_large"));
		HandleFailed(RequestId, Reason, PendingAssistantText);
		return;
	}

	CommittedHistory.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
		EUnrealAIMessageRole::User,
		PendingUserText));
	CommittedHistory.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
		EUnrealAIMessageRole::Assistant,
		NormalizedFinalText));
	TrimCommittedHistoryToLimits();
	ClearActiveState(EUnrealAIPackagedChatState::Idle);
	RetryUserText.Reset();
	InterruptedAssistantText.Reset();
	LastPublicFailureReason = NAME_None;
	ReceiveCompleted(NormalizedFinalText);
}

void UUnrealAIPackagedChatWidgetExample::HandleFailed(
	FGuid RequestId,
	FName PublicReason,
	const FString& PartialText)
{
	if (!IsActiveEvent(RequestId))
	{
		return;
	}

	const bool bSessionExpired = PublicReason == TEXT("session_expired");
	const bool bRetryAllowed = PublicReason != TEXT("access_denied");
	RetryUserText = bRetryAllowed ? PendingUserText : FString();
	InterruptedAssistantText = UnrealAISampleStringBounds::LeftAtCodePointBoundary(
		PartialText,
		UnrealAIPackagedChatWidgetExample::MaxMessageCharacters);
	LastPublicFailureReason = PublicReason.IsNone()
		? FName(TEXT("service_unavailable"))
		: PublicReason;
	if (bSessionExpired)
	{
		bBackendReady = false;
		ReceiveBackendReadinessChanged(false);
	}
	ClearActiveState(EUnrealAIPackagedChatState::Failed);
	ReceiveFailed(LastPublicFailureReason, InterruptedAssistantText);
}

void UUnrealAIPackagedChatWidgetExample::HandleCancelled(
	FGuid RequestId,
	const FString& PartialText)
{
	if (!IsActiveEvent(RequestId))
	{
		return;
	}

	RetryUserText = PendingUserText;
	InterruptedAssistantText = UnrealAISampleStringBounds::LeftAtCodePointBoundary(
		PartialText,
		UnrealAIPackagedChatWidgetExample::MaxMessageCharacters);
	LastPublicFailureReason = NAME_None;
	ClearActiveState(EUnrealAIPackagedChatState::Cancelled);
	ReceiveCancelled(InterruptedAssistantText);
}

bool UUnrealAIPackagedChatWidgetExample::IsActiveEvent(const FGuid& RequestId) const
{
	return bAcceptEvents && ActiveRequestId.IsValid() && RequestId == ActiveRequestId;
}

void UUnrealAIPackagedChatWidgetExample::ReconcileRejectedCancel(
	const FGuid& CancelledRequestId,
	EUnrealAIPackagedChatState PreviousState)
{
	// Cancellation can synchronously drain a terminal event and clear or
	// replace the request. Restore only if the same request is still active.
	if (IsActiveEvent(CancelledRequestId))
	{
		RequestState = PreviousState;
	}
}

void UUnrealAIPackagedChatWidgetExample::ClearActiveState(
	EUnrealAIPackagedChatState NewState)
{
	ActiveRequestId.Invalidate();
	PendingUserText.Reset();
	PendingAssistantText.Reset();
	RequestState = NewState;
}

void UUnrealAIPackagedChatWidgetExample::TrimHistoryForRequest(
	TArray<FUnrealAIChatMessage>& History,
	const FString& PendingUser)
{
	using namespace UnrealAIPackagedChatWidgetExample;
	while (History.Num() >= MaxCommittedMessages
		|| (!History.IsEmpty()
			&& CountHistoryCharacters(History) + PendingUser.Len() > MaxTotalCharacters))
	{
		if (History.Num() < 2)
		{
			History.Reset();
			break;
		}
		History.RemoveAt(0, 2, EAllowShrinking::No);
	}
}

void UUnrealAIPackagedChatWidgetExample::TrimCommittedHistoryToLimits()
{
	using namespace UnrealAIPackagedChatWidgetExample;
	while (CommittedHistory.Num() > MaxCommittedMessages
		|| CountHistoryCharacters(CommittedHistory) > MaxTotalCharacters)
	{
		if (CommittedHistory.Num() < 2)
		{
			CommittedHistory.Reset();
			break;
		}
		CommittedHistory.RemoveAt(0, 2, EAllowShrinking::No);
	}
}

int32 UUnrealAIPackagedChatWidgetExample::CountHistoryCharacters(
	const TArray<FUnrealAIChatMessage>& Messages)
{
	int32 Total = 0;
	for (const FUnrealAIChatMessage& Message : Messages)
	{
		Total += Message.Content.Len();
	}
	return Total;
}
