#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UnrealAITypes.h"
#include "UnrealAIPackagedClientFacadeExample.generated.h"

class UUnrealAIClient;
struct FUnrealAIPackagedClientFacadeExampleTestAccess;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FUnrealAIGameChatTextDeltaEvent,
	FGuid,
	RequestId,
	const FString&,
	TextDelta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(
	FUnrealAIGameChatRetryingEvent,
	FGuid,
	RequestId,
	int32,
	RetryNumber,
	int32,
	MaxRetries,
	float,
	DelaySeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FUnrealAIGameChatCompletedEvent,
	FGuid,
	RequestId,
	const FString&,
	FinalText);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FUnrealAIGameChatFailedEvent,
	FGuid,
	RequestId,
	FName,
	PublicReason,
	const FString&,
	PartialText);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FUnrealAIGameChatCancelledEvent,
	FGuid,
	RequestId,
	const FString&,
	PartialText);

/**
 * Compile-tested packaged-client boundary for an authenticated OpenAI-compatible game backend.
 * Native game/session code creates and initializes this object; Blueprint cannot set credentials,
 * provider URLs, or model names. Reflected advanced message fields are rejected at this boundary.
 */
UCLASS(BlueprintType)
class UNREALAISAMPLE_API UUnrealAIPackagedClientFacadeExample : public UObject
{
	GENERATED_BODY()

public:
	/** Native-only initialization keeps the short-lived game-session token off reflected pins. */
	bool Initialize(const FString& ShortLivedGameSessionToken, FUnrealAIError& OutError);

	/** Native-only readiness check used by the trusted session/controller boundary. */
	bool IsInitialized() const { return BackendClient != nullptr; }

	/** Starts one serialized stream through the trusted backend. */
	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Game Backend")
	bool StreamTurn(
		FGuid RequestId,
		const TArray<FUnrealAIChatMessage>& Messages,
		FName& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Game Backend")
	bool CancelActiveTurn();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Game Backend")
	bool bRequestInFlight = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Game Backend")
	FGuid ActiveRequestId;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Game Backend")
	FUnrealAIGameChatTextDeltaEvent OnTextDelta;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Game Backend")
	FUnrealAIGameChatRetryingEvent OnRetrying;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Game Backend")
	FUnrealAIGameChatCompletedEvent OnCompleted;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Game Backend")
	FUnrealAIGameChatFailedEvent OnFailed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI Example|Game Backend")
	FUnrealAIGameChatCancelledEvent OnCancelled;

protected:
	virtual void BeginDestroy() override;

private:
	friend struct FUnrealAIPackagedClientFacadeExampleTestAccess;

	UPROPERTY()
	TObjectPtr<UUnrealAIClient> BackendClient;

	FUnrealAIRequestHandle ActiveRequest;
	FString PartialText;
	bool bOutputLimitExceeded = false;
	bool bShuttingDown = false;

#if WITH_DEV_AUTOMATION_TESTS
	int32 TerminalBroadcastCountForTests = 0;
	FName LastTerminalFailureReasonForTests;
#endif

	static bool ValidateMessages(const TArray<FUnrealAIChatMessage>& Messages, FName& OutFailureReason);
	static FUnrealAIChatRequest BuildBackendRequest(
		const FGuid& RequestId,
		const TArray<FUnrealAIChatMessage>& Messages);
	static FName MapFailureReason(const FUnrealAIChatStreamResult& Result);
	void ClearActiveRequest();
	void HandleStreamEvent(const FUnrealAIChatStreamEvent& Event);
	void HandleRetry(const FUnrealAIRetryEvent& RetryEvent);
	void HandleTerminal(const FUnrealAIChatStreamResult& Result);
};
