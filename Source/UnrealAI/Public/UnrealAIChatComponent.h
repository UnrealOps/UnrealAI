#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UnrealAIClient.h"
#include "UnrealAIChatComponent.generated.h"

#if WITH_DEV_AUTOMATION_TESTS
struct FUnrealAIChatComponentTestAccess;
#endif

UCLASS(ClassGroup = (AI), meta = (BlueprintSpawnableComponent))
class UNREALAI_API UUnrealAIChatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UUnrealAIChatComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI")
	FName ProviderName = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI")
	FString Model;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI", meta = (MultiLine = true))
	FString SystemPrompt;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Sampling")
	bool bUseTemperature = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Sampling", meta = (EditCondition = "bUseTemperature", ClampMin = "0.0", ClampMax = "2.0"))
	float Temperature = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability")
	FUnrealAIRequestRetryOptions RetryOptions;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin OnChatCompleted;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin OnChatFailed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin OnChatCancelled;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Retry")
	FUnrealAIRetryPin OnChatRetrying;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Streaming")
	FUnrealAIChatStreamEventPin OnChatStreamEvent;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Streaming")
	FUnrealAIChatCompletionPin OnChatStreamCompleted;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Streaming")
	FUnrealAIChatCompletionPin OnChatStreamFailed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Streaming")
	FUnrealAIChatStreamCancelledPin OnChatStreamCancelled;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Streaming|Retry")
	FUnrealAIRetryPin OnChatStreamRetrying;

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat")
	void SendPrompt(const FString& Prompt);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat")
	void SendMessages(const TArray<FUnrealAIChatMessage>& Messages);

	/** Cancels every one-shot completion currently owned by this component. */
	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat")
	int32 CancelActiveCompletions();

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat|Streaming")
	void SendPromptStream(const FString& Prompt);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat|Streaming")
	void SendMessagesStream(const TArray<FUnrealAIChatMessage>& Messages);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat|Streaming")
	bool CancelActiveStream();

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend struct FUnrealAIChatComponentTestAccess;
#endif

	UPROPERTY()
	TObjectPtr<UUnrealAIClient> Client;

	FUnrealAIRequestHandle ActiveStreamHandle;
	TMap<FGuid, FUnrealAIRequestHandle> ActiveCompletionHandles;
	bool bEndingPlay = false;

	TArray<FUnrealAIChatMessage> BuildPromptMessages(const FString& Prompt) const;
	FUnrealAIChatRequest BuildRequest(const TArray<FUnrealAIChatMessage>& Messages) const;
	bool EnsureClient(FUnrealAIError& OutError);
	void HandleCompletion(FGuid CompletionId, const FUnrealAIChatResponse& Response, const FUnrealAIError& Error);
	void HandleRetry(const FUnrealAIRetryEvent& RetryEvent);
	void HandleStreamEvent(const FUnrealAIChatStreamEvent& Event);
	void HandleStreamRetry(const FUnrealAIRetryEvent& RetryEvent);
	void HandleStreamTerminal(const FUnrealAIChatStreamResult& Result);
};
