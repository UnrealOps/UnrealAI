#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "UnrealAITypes.h"
#include "UnrealAIClient.generated.h"

struct FUnrealAIStreamRequestState;

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

	void CreateChatCompletion(const FUnrealAIChatRequest& Request, FUnrealAIChatCompletionNativeDelegate CompletionDelegate);
	FUnrealAIRequestHandle StreamChatCompletion(
		const FUnrealAIChatRequest& Request,
		FUnrealAIChatStreamEventNativeDelegate EventDelegate,
		FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate);
	bool CancelRequest(const FUnrealAIRequestHandle& RequestHandle);

	virtual void BeginDestroy() override;

private:
	FUnrealAIProviderConfig ProviderConfig;
	bool bConfigured = false;
	TArray<FHttpRequestPtr> InFlightRequests;
	TMap<FGuid, TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>> ActiveStreamRequests;

	FString ResolveApiKey() const;
	void HandleChatCompletionResponse(
		FHttpRequestPtr HttpRequest,
		FHttpResponsePtr HttpResponse,
		bool bWasSuccessful,
		EUnrealAIProviderApi ProviderApi,
		FString ResolvedModel,
		FUnrealAIChatCompletionNativeDelegate CompletionDelegate);
	void DrainStreamRequest(const FGuid& RequestId);
	void HandleStreamResponse(
		FHttpRequestPtr HttpRequest,
		FHttpResponsePtr HttpResponse,
		bool bWasSuccessful,
		FGuid RequestId);
	void CompleteStreamRequest(
		const TSharedPtr<FUnrealAIStreamRequestState, ESPMode::ThreadSafe>& State,
		EUnrealAIChatStreamStatus Status,
		const FUnrealAIError& Error);
	void CancelAllStreams();
};
