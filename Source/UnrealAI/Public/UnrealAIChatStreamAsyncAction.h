#pragma once

#include "CoreMinimal.h"
#include "Engine/CancellableAsyncAction.h"
#include "UnrealAIClient.h"
#include "UnrealAIChatStreamAsyncAction.generated.h"

UCLASS(meta = (ExposedAsyncProxy = "AsyncAction"))
class UNREALAI_API UUnrealAIChatStreamAsyncAction : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatStreamEventPin Event;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin Completed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin Failed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatStreamCancelledPin Cancelled;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Retry")
	FUnrealAIRetryPin Retrying;

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Stream Chat Completion (UnrealAI)"))
	static UUnrealAIChatStreamAsyncAction* StreamChatCompletion(
		UObject* WorldContextObject,
		FName ProviderName,
		const FUnrealAIChatRequest& Request);

	virtual void Activate() override;
	virtual void Cancel() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContext;

	UPROPERTY()
	TObjectPtr<UUnrealAIClient> Client;

	FName Provider;
	FUnrealAIChatRequest PendingRequest;
	FUnrealAIRequestHandle RequestHandle;
	bool bTerminal = false;

	void HandleEvent(const FUnrealAIChatStreamEvent& StreamEvent);
	void HandleRetry(const FUnrealAIRetryEvent& RetryEvent);
	void HandleTerminal(const FUnrealAIChatStreamResult& Result);
	void BroadcastFailure(const FUnrealAIError& Error);
};
