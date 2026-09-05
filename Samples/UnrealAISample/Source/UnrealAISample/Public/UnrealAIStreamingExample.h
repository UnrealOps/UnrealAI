#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UnrealAITypes.h"
#include "UnrealAIStreamingExample.generated.h"

class UUnrealAIClient;
struct FUnrealAIStreamingExampleTestAccess;

UCLASS()
class UNREALAISAMPLE_API AUnrealAIStreamingExample : public AActor
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void StartStreaming();

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void CancelStreaming();

	UPROPERTY(EditAnywhere, Category = "UnrealAI Example")
	FName ProviderName = TEXT("OpenAI");

	UPROPERTY(EditAnywhere, Category = "UnrealAI Example", meta = (MultiLine = true))
	FString Prompt = TEXT("Describe this level in one sentence.");

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI Example")
	FString StreamingText;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend struct FUnrealAIStreamingExampleTestAccess;

	UPROPERTY()
	TObjectPtr<UUnrealAIClient> UnrealAIClient;

	FUnrealAIRequestHandle ActiveStream;

	void HandleStreamEvent(const FUnrealAIChatStreamEvent& Event);
	void HandleStreamRetry(const FUnrealAIRetryEvent& RetryEvent);
	void HandleStreamTerminal(const FUnrealAIChatStreamResult& Result);
};
