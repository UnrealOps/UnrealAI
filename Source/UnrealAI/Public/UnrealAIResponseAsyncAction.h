#pragma once

#include "CoreMinimal.h"
#include "Engine/CancellableAsyncAction.h"
#include "UnrealAIClient.h"
#include "UnrealAIResponseAsyncAction.generated.h"

UCLASS(meta = (ExposedAsyncProxy = "AsyncAction"))
class UNREALAI_API UUnrealAIResponseAsyncAction : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Responses")
	FUnrealAIResponseActionPin Completed;
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Responses")
	FUnrealAIResponseActionPin Incomplete;
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Responses")
	FUnrealAIResponseActionPin Failed;
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Responses")
	FUnrealAIResponseActionPin Cancelled;
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Responses")
	FUnrealAIResponseActionPin Retrying;
	UPROPERTY(BlueprintAssignable, Category = "UnrealAI|Responses")
	FUnrealAIResponseActionPin Event;

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Responses", meta = (BlueprintInternalUseOnly = "true",
		WorldContext = "WorldContextObject", DisplayName = "Create Response (UnrealAI)"))
	static UUnrealAIResponseAsyncAction* CreateResponse(UObject* WorldContextObject,
		FName ProviderName, const FUnrealAIResponseRequest& Request);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Responses", meta = (BlueprintInternalUseOnly = "true",
		WorldContext = "WorldContextObject", DisplayName = "Stream Response (UnrealAI)"))
	static UUnrealAIResponseAsyncAction* StreamResponse(UObject* WorldContextObject,
		FName ProviderName, const FUnrealAIResponseRequest& Request);

	virtual void Activate() override;
	virtual void Cancel() override;

private:
	UPROPERTY()
	TObjectPtr<UUnrealAIClient> Client;
	UPROPERTY()
	TObjectPtr<UObject> WorldContext;
	FName Provider;
	FUnrealAIResponseRequest Request;
	FUnrealAIRequestHandle Handle;
	bool bStreaming = false;
	bool bActivated = false;
	bool bTerminal = false;
	void HandleTerminal(const FUnrealAIResponseResult& Result);
	void HandleEvent(const FUnrealAIResponseEvent& ResponseEvent);
	void HandleRetry(const FUnrealAIRetryEvent& RetryEvent);
};
