#pragma once

#include "CoreMinimal.h"
#include "Engine/CancellableAsyncAction.h"
#include "UnrealAIClient.h"
#include "UnrealAIChatCompletionAsyncAction.generated.h"

UCLASS(meta = (ExposedAsyncProxy = "AsyncAction"))
class UNREALAI_API UUnrealAIChatCompletionAsyncAction : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin Completed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin Failed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin Cancelled;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Retry")
	FUnrealAIRetryPin Retrying;

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Create Chat Completion (UnrealAI)"))
	static UUnrealAIChatCompletionAsyncAction* CreateChatCompletion(UObject* WorldContextObject, FName ProviderName, const FUnrealAIChatRequest& Request);

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

	void HandleCompletion(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error);
	void HandleRetry(const FUnrealAIRetryEvent& RetryEvent);
	void BroadcastFailure(const FUnrealAIError& Error);
};
