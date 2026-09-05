#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UnrealAITypes.h"
#include "UnrealAISampleActor.generated.h"

class UUnrealAIClient;

/**
 * Minimal native consumer that demonstrates one-shot and streaming UnrealAI requests.
 * API keys are resolved by UnrealAI from the process environment or the project .env file.
 */
UCLASS(Blueprintable)
class UNREALAISAMPLE_API AUnrealAISampleActor : public AActor
{
	GENERATED_BODY()

public:
	AUnrealAISampleActor();

	/** Uses the provider-specific XAI() factory and submits a one-shot request. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "UnrealAI Sample|C++")
	void RunCppCompletionSample();

	/** Uses the same retained client and the dedicated SSE streaming API. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "UnrealAI Sample|C++")
	void RunCppStreamingSample();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "UnrealAI Sample|C++")
	void CancelCppRequests();

	/** Terminal sinks used by the generated Blueprint example and live validation. */
	UFUNCTION(BlueprintCallable, Category = "UnrealAI Sample|Blueprint")
	void RecordBlueprintSuccess(const FString& Content, bool bHasContent);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Sample|Blueprint")
	void RecordBlueprintFailure(const FUnrealAIError& Error);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Sample|Blueprint")
	void RecordBlueprintCancellation();

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Sample|Blueprint")
	void RecordBlueprintRetry();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI Sample")
	FString Prompt = TEXT("Reply with exactly: UnrealAI sample works.");

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|C++")
	bool bCppCompletionFinished = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|C++")
	bool bCppCompletionSucceeded = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|C++")
	FString CppCompletionText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|C++")
	FString CppStreamingText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|C++")
	bool bCppStreamFinished = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|C++")
	bool bCppStreamSucceeded = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|C++")
	int32 CppStreamTextEventCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|C++")
	int32 CppStreamTerminalCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|Blueprint")
	bool bBlueprintRequestFinished = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|Blueprint")
	bool bBlueprintRequestSucceeded = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample|Blueprint")
	FString BlueprintResponseText;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "UnrealAI Sample")
	FString LastStatus;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY()
	TObjectPtr<UUnrealAIClient> UnrealAIClient;

	FUnrealAIRequestHandle ActiveCompletion;
	FUnrealAIRequestHandle ActiveStream;

	bool EnsureXAIClient();
	void HandleCompletion(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error);
	void HandleRetry(const FUnrealAIRetryEvent& RetryEvent);
	void HandleStreamEvent(const FUnrealAIChatStreamEvent& Event);
	void HandleStreamTerminal(const FUnrealAIChatStreamResult& Result);
};
