#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "UnrealAITypes.h"
#include "UnrealAIClient.generated.h"

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

private:
	FUnrealAIProviderConfig ProviderConfig;
	bool bConfigured = false;
	TArray<FHttpRequestPtr> InFlightRequests;

	FString ResolveApiKey() const;
	FString BuildEndpointUrl(const FString& Path) const;
	bool BuildChatCompletionPayload(const FUnrealAIChatRequest& Request, FString& OutPayload, FUnrealAIError& OutError) const;
	void HandleChatCompletionResponse(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bWasSuccessful, FUnrealAIChatCompletionNativeDelegate CompletionDelegate);
};
