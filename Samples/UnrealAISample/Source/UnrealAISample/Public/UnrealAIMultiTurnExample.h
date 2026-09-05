#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UnrealAITypes.h"
#include "UnrealAIMultiTurnExample.generated.h"

class UUnrealAIClient;
struct FUnrealAIMultiTurnExampleTestAccess;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FUnrealAIMixedTurnCompletedEvent,
	const FString&,
	AssistantText);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FUnrealAIMixedTurnFailedEvent,
	const FUnrealAIError&,
	Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FUnrealAIMixedTurnCancelledEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FUnrealAIMixedTurnRetryingEvent,
	const FUnrealAIRetryEvent&,
	RetryEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FUnrealAIMixedTurnTextDeltaEvent,
	const FString&,
	TextDelta);

/**
 * Multi-turn recipe in which C++ owns provider access and conversation state,
 * while a Blueprint subclass or UI binds to lifecycle events.
 */
UCLASS(Blueprintable)
class UNREALAISAMPLE_API AUnrealAIMultiTurnExample : public AActor
{
	GENERATED_BODY()

public:
	/** Starts an empty conversation and inserts SystemPrompt once when non-empty. */
	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Conversation")
	bool ResetConversation();

	/** Serializes turns so responses cannot be committed to the wrong prompt. */
	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Conversation")
	bool SendTurn(const FString& UserText, FUnrealAIError& OutError);

	/** Streams one serialized turn and commits only the successful aggregate response. */
	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Conversation")
	bool SendTurnStream(const FString& UserText, FUnrealAIError& OutError);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Conversation")
	bool CancelTurn();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI Example|Configuration")
	FName ProviderName = TEXT("OpenAI");

	/** Applied by ResetConversation; change it only between conversations. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI Example|Configuration", meta = (MultiLine = true))
	FString SystemPrompt = TEXT("You are a concise in-game guide.");

	/** Number of completed user/assistant pairs retained before the next request. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI Example|Conversation", meta = (ClampMin = "1"))
	int32 MaxRetainedTurns = 8;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Conversation")
	TArray<FUnrealAIChatMessage> ConversationHistory;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Conversation")
	bool bRequestInFlight = false;

	/** Temporary text for the active stream; it is never committed message-by-message. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Conversation")
	FString PendingAssistantText;

	/** Display-only partial text from the most recent failed or cancelled stream. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Conversation")
	FString LastInterruptedAssistantText;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Conversation")
	FUnrealAIMixedTurnCompletedEvent OnTurnCompleted;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Conversation")
	FUnrealAIMixedTurnFailedEvent OnTurnFailed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Conversation")
	FUnrealAIMixedTurnCancelledEvent OnTurnCancelled;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Conversation")
	FUnrealAIMixedTurnRetryingEvent OnTurnRetrying;

	/** Normalized text only; raw provider events are intentionally not exposed. */
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Conversation")
	FUnrealAIMixedTurnTextDeltaEvent OnTurnTextDelta;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend struct FUnrealAIMultiTurnExampleTestAccess;

	UPROPERTY()
	TObjectPtr<UUnrealAIClient> UnrealAIClient;

	FUnrealAIRequestHandle ActiveRequest;
	FUnrealAIChatMessage PendingUserMessage;
	bool bStreamingTurn = false;
	bool bEndingPlay = false;

	bool EnsureClient(FUnrealAIError& OutError);
	bool PrepareTurn(const FString& UserText, FUnrealAIChatRequest& OutRequest, FUnrealAIError& OutError);
	void TrimCompletedHistory();
	void RollBackPendingUserTurn();
	bool CommitAssistantResponse(const FUnrealAIChatResponse& Response, FString& OutAssistantText);
	static void SetLocalError(FUnrealAIError& OutError, const FString& Code, const FString& Message);
	void BroadcastLocalError(const FString& Code, const FString& Message);
	void HandleTurnCompleted(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error);
	void HandleTurnRetry(const FUnrealAIRetryEvent& RetryEvent);
	void HandleStreamEvent(const FUnrealAIChatStreamEvent& Event);
	void HandleStreamTerminal(const FUnrealAIChatStreamResult& Result);
};
