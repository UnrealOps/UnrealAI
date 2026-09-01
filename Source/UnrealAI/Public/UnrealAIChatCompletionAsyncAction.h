#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "UnrealAIClient.h"
#include "UnrealAIChatCompletionAsyncAction.generated.h"

UCLASS()
class UNREALAI_API UUnrealAIChatCompletionAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin Completed;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin Failed;

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat", meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Create Chat Completion (UnrealAI)"))
	static UUnrealAIChatCompletionAsyncAction* CreateChatCompletion(UObject* WorldContextObject, FName ProviderName, const FUnrealAIChatRequest& Request);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContext;

	UPROPERTY()
	TObjectPtr<UUnrealAIClient> Client;

	FName Provider;
	FUnrealAIChatRequest PendingRequest;

	void HandleCompletion(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error);
	void BroadcastFailure(const FUnrealAIError& Error);
};
