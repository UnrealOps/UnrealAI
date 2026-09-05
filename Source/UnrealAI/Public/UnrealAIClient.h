#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "UnrealAITypes.h"
#include "UnrealAIResponseTypes.h"
#include "UnrealAIClient.generated.h"

struct FUnrealAIRequestState;
struct FUnrealAISseEvent;
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

	/**
	 * Starts one logical request. Call from the game thread. Completion and retry
	 * delegates are delivered on the game thread; validation failures may invoke
	 * completion synchronously before this function returns.
	 */
	FUnrealAIRequestHandle CreateChatCompletion(
		const FUnrealAIChatRequest& Request,
		FUnrealAIChatCompletionNativeDelegate CompletionDelegate,
		FUnrealAIRetryNativeDelegate RetryDelegate = FUnrealAIRetryNativeDelegate());
	/**
	 * Starts one logical SSE stream. Call from the game thread. Event, terminal,
	 * and retry delegates are delivered on the game thread; preflight failures may
	 * invoke the terminal delegate synchronously before this function returns.
	 */
	FUnrealAIRequestHandle StreamChatCompletion(
		const FUnrealAIChatRequest& Request,
		FUnrealAIChatStreamEventNativeDelegate EventDelegate,
		FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate,
		FUnrealAIRetryNativeDelegate RetryDelegate = FUnrealAIRetryNativeDelegate());
	/** Cancels on the game thread; a terminal delegate may run synchronously. */
	bool CancelRequest(const FUnrealAIRequestHandle& RequestHandle);

	/** One provider turn; tool execution and subsequent turns belong to the caller.
	 * Callbacks run on the game thread. Preflight failures may complete synchronously.
	 */
	FUnrealAIRequestHandle CreateResponse(const FUnrealAIResponseRequest& Request,
		FUnrealAIResponseNativeDelegate CompletionDelegate,
		FUnrealAIRetryNativeDelegate RetryDelegate = FUnrealAIRetryNativeDelegate());

	/** Streams ordered response items, followed by exactly one terminal callback. */
	FUnrealAIRequestHandle StreamResponse(const FUnrealAIResponseRequest& Request,
		FUnrealAIResponseEventNativeDelegate EventDelegate,
		FUnrealAIResponseNativeDelegate TerminalDelegate,
		FUnrealAIRetryNativeDelegate RetryDelegate = FUnrealAIRetryNativeDelegate());

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Responses")
	FUnrealAIResponseCapabilities GetResponseCapabilities() const;

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Responses")
	bool ValidateResponseRequest(const FUnrealAIResponseRequest& Request, FUnrealAIError& OutError) const;

	virtual void BeginDestroy() override;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend struct FUnrealAIClientTestAccess;
#endif

	FUnrealAIProviderConfig ProviderConfig;
	bool bConfigured = false;
	TMap<FGuid, TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>> ActiveRequests;

	FString ResolveApiKey() const;
	FUnrealAIRequestHandle StartResponse(const FUnrealAIResponseRequest& Request, bool bStream,
		FUnrealAIResponseEventNativeDelegate EventDelegate, FUnrealAIResponseNativeDelegate TerminalDelegate,
		FUnrealAIRetryNativeDelegate RetryDelegate);
	bool ProcessSseEvent(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
		const FUnrealAISseEvent& Frame, FUnrealAIError& OutError);
	void DeliverResponseTerminal(const TSharedPtr<FUnrealAIRequestState, ESPMode::ThreadSafe>& State,
		bool bCancelled, const FUnrealAIError& Error);
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
