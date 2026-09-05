#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "UnrealAITypes.h"
#include "UnrealAIPackagedChatWidgetExample.generated.h"

class UUnrealAIPackagedClientFacadeExample;
struct FUnrealAIPackagedChatWidgetExampleTestAccess;

UENUM(BlueprintType)
enum class EUnrealAIPackagedChatState : uint8
{
	Unavailable,
	Idle,
	Starting,
	Streaming,
	Retrying,
	Cancelling,
	Failed,
	Cancelled,
	Destroying
};

/**
 * Compile-tested state owner for a packaged Widget Blueprint that talks only to
 * an authenticated game backend. Native session code injects the initialized
 * facade; provider configuration and session credentials never become pins.
 */
UCLASS(Blueprintable)
class UNREALAISAMPLE_API UUnrealAIPackagedChatWidgetExample : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Native-only injection; rejects a facade that has not completed Initialize. */
	bool AttachInitializedFacade(UUnrealAIPackagedClientFacadeExample* InFacade);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Packaged Chat")
	bool SubmitTurn(const FString& UserText, FName& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Packaged Chat")
	bool RetryLastTurn(FName& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example|Packaged Chat")
	bool CancelTurn();

	/** Contains completed user/assistant pairs only; the backend owns the system prompt. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Packaged Chat")
	TArray<FUnrealAIChatMessage> CommittedHistory;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Packaged Chat")
	EUnrealAIPackagedChatState RequestState = EUnrealAIPackagedChatState::Unavailable;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Packaged Chat")
	FGuid ActiveRequestId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Packaged Chat")
	FString PendingAssistantText;

	/** Display-only text from the most recent interrupted turn. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Packaged Chat")
	FString InterruptedAssistantText;

	/** Allow-listed reason for the most recent failed turn; never a raw provider error. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Packaged Chat")
	FName LastPublicFailureReason;

	/** False after session expiry until native code attaches a refreshed facade. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Example|Packaged Chat")
	bool bBackendReady = false;

	/** Generated Widget Blueprints implement these presentation-only routes. */
	UFUNCTION(BlueprintImplementableEvent, Category = "UnrealAI Example|Packaged Chat")
	void ReceiveBackendReadinessChanged(bool bIsReady);

	UFUNCTION(BlueprintImplementableEvent, Category = "UnrealAI Example|Packaged Chat")
	void ReceiveTextDelta(const FString& TextDelta);

	UFUNCTION(BlueprintImplementableEvent, Category = "UnrealAI Example|Packaged Chat")
	void ReceiveRetrying(int32 RetryNumber, int32 MaxRetries, float DelaySeconds);

	UFUNCTION(BlueprintImplementableEvent, Category = "UnrealAI Example|Packaged Chat")
	void ReceiveCompleted(const FString& FinalText);

	UFUNCTION(BlueprintImplementableEvent, Category = "UnrealAI Example|Packaged Chat")
	void ReceiveFailed(FName PublicReason, const FString& PartialText);

	UFUNCTION(BlueprintImplementableEvent, Category = "UnrealAI Example|Packaged Chat")
	void ReceiveCancelled(const FString& PartialText);

protected:
	virtual void NativeDestruct() override;

private:
	friend struct FUnrealAIPackagedChatWidgetExampleTestAccess;

	UPROPERTY(Transient)
	TObjectPtr<UUnrealAIPackagedClientFacadeExample> Facade;

	FString PendingUserText;
	FString RetryUserText;
	bool bAcceptEvents = false;

#if WITH_DEV_AUTOMATION_TESTS
	/** Deterministic transport seam for exercising synchronous controller paths. */
	TFunction<bool(const FGuid&, const TArray<FUnrealAIChatMessage>&, FName&)>
		StreamTurnForTests;
#endif

	UFUNCTION()
	void HandleTextDelta(FGuid RequestId, const FString& TextDelta);

	UFUNCTION()
	void HandleRetrying(FGuid RequestId, int32 RetryNumber, int32 MaxRetries, float DelaySeconds);

	UFUNCTION()
	void HandleCompleted(FGuid RequestId, const FString& FinalText);

	UFUNCTION()
	void HandleFailed(FGuid RequestId, FName PublicReason, const FString& PartialText);

	UFUNCTION()
	void HandleCancelled(FGuid RequestId, const FString& PartialText);

	bool IsActiveEvent(const FGuid& RequestId) const;
	void ReconcileRejectedCancel(
		const FGuid& CancelledRequestId,
		EUnrealAIPackagedChatState PreviousState);
	void ClearActiveState(EUnrealAIPackagedChatState NewState);
	static void TrimHistoryForRequest(
		TArray<FUnrealAIChatMessage>& History,
		const FString& PendingUser);
	void TrimCommittedHistoryToLimits();
	static int32 CountHistoryCharacters(const TArray<FUnrealAIChatMessage>& Messages);
};
