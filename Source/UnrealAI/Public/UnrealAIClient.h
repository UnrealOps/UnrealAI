#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "UnrealAITypes.h"
#include "UnrealAIClient.generated.h"

struct FUnrealAIRequestState;
#if WITH_DEV_AUTOMATION_TESTS
struct FUnrealAIClientTestAccess;
#endif

UCLASS(BlueprintType)
class UNREALAI_API UUnrealAIClient : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "UnrealAI")
	void Configure(const FUnrealAIProviderConfig& InProviderConfig);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI")
	bool ConfigureFromSettings(FName ProviderName, FUnrealAIError& OutError);

	UFUNCTION(BlueprintPure, Category = "UnrealAI")
	bool IsConfigured() const;

	UFUNCTION(BlueprintPure, Category = "UnrealAI")
	const FUnrealAIProviderConfig& GetProviderConfig() const;

	FUnrealAIRequestHandle CreateChatCompletion(
		const FUnrealAIChatRequest& Request,
		FUnrealAIChatCompletionNativeDelegate CompletionDelegate,
		FUnrealAIRetryNativeDelegate RetryDelegate = FUnrealAIRetryNativeDelegate());
	FUnrealAIRequestHandle StreamChatCompletion(
		const FUnrealAIChatRequest& Request,
		FUnrealAIChatStreamEventNativeDelegate EventDelegate,
		FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate,
		FUnrealAIRetryNativeDelegate RetryDelegate = FUnrealAIRetryNativeDelegate());
	bool CancelRequest(const FUnrealAIRequestHandle& RequestHandle);

	virtual void BeginDestroy() override;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend struct FUnrealAIClientTestAccess;
#endif

	FUnrealAIProviderConfig ProviderConfig;
	bool bConfigured = false;
	TMap<FGuid, TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>> ActiveRequests;

	FString ResolveApiKey() const;
	void StartRequestAttempt(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State);
	void ResumeRequestAfterBackoff(const FGuid& RequestId);
	void HandleChatCompletionResponse(
		FHttpRequestPtr HttpRequest,
		FHttpResponsePtr HttpResponse,
		bool bWasSuccessful,
		FGuid RequestId,
		int32 AttemptNumber);
	void DrainStreamRequest(const FGuid& RequestId);
	void HandleStreamResponse(
		FHttpRequestPtr HttpRequest,
		FHttpResponsePtr HttpResponse,
		bool bWasSuccessful,
		FGuid RequestId,
		int32 AttemptNumber);
	bool TryScheduleRetry(
		const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
		EUnrealAIRetryReason Reason,
		int32 HttpStatus,
		const FUnrealAIError& Error,
		const FHttpResponsePtr& HttpResponse);
	void CompleteOneShotRequest(
		const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
		const FUnrealAIChatResponse& Response,
		const FUnrealAIError& Error);
	void CompleteStreamRequest(
		const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
		EUnrealAIChatStreamStatus Status,
		const FUnrealAIError& Error);
	void CancelAllRequests();
};
