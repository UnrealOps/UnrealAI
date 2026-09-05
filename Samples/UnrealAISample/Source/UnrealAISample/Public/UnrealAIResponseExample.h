#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UnrealAIResponseTypes.h"
#include "UnrealAIResponseExample.generated.h"

class UUnrealAIClient;
class UUnrealAIResponseAsyncAction;

/** Bounded, read-only tool example. Call on a trusted server or in standalone development. */
UCLASS(Blueprintable)
class UNREALAISAMPLE_API AUnrealAIResponseExample : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI Example")
	FName ProviderName = TEXT("OpenAI");

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void StartNativeResponses(bool bStream = false);

	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "UnrealAI Example")
	void StartBlueprintResponses();

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void CancelResponses();

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	bool PrepareResponseRequest(FUnrealAIResponseRequest& OutRequest, FName& OutProvider);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	bool PrepareToolContinuation(const FUnrealAIResponseResult& Result, FUnrealAIResponseRequest& OutRequest);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void RecordResponseTerminal(const FUnrealAIResponseResult& Result);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void RecordResponseEvent(const FUnrealAIResponseEvent& ResponseEvent);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void RecordResponseRetry(const FUnrealAIRetryEvent& RetryEvent);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void RetainResponseAction(UUnrealAIResponseAsyncAction* Action);

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI Example")
	FString DisplayText;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI Example")
	FUnrealAIResponseResult LastResult;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI Example")
	bool bDone = true;

	int32 ExecutedToolCount = 0;
	int32 TerminalCount = 0;
	int32 EventCount = 0;
	int32 RetryCount = 0;
	bool bCallbacksOnGameThread = true;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY()
	TObjectPtr<UUnrealAIClient> Client;
	UPROPERTY()
	TObjectPtr<UUnrealAIResponseAsyncAction> ActiveAction;
	FUnrealAIRequestHandle ActiveHandle;
	FUnrealAIResponseRequest CurrentRequest;
	bool bStreaming = false;
	int32 SubmissionSerial = 0;

	void SubmitNative(bool bContinuation);
	void HandleFirstResponse(const FUnrealAIResponseResult& Result);
	bool FailExample(const TCHAR* Message);
};
