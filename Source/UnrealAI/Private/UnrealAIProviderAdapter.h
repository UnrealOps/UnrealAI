#pragma once

#include "CoreMinimal.h"
#include "UnrealAITypes.h"

struct FUnrealAIHttpRequestData
{
	FString Url;
	FString Verb = TEXT("POST");
	TMap<FString, FString> Headers;
	FString Body;
	FString ResolvedModel;
};

class IUnrealAIProviderAdapter
{
public:
	virtual ~IUnrealAIProviderAdapter() = default;

	virtual bool BuildRequest(
		const FUnrealAIProviderConfig& ProviderConfig,
		const FUnrealAIChatRequest& Request,
		const FString& ApiKey,
		FUnrealAIHttpRequestData& OutRequest,
		FUnrealAIError& OutError) const = 0;

	virtual void ParseResponse(
		const FString& ResolvedModel,
		int32 HttpStatus,
		const FString& RawJson,
		FUnrealAIChatResponse& OutResponse,
		FUnrealAIError& OutError) const = 0;
};

namespace UnrealAIProviderAdapters
{
	const IUnrealAIProviderAdapter& Get(EUnrealAIProviderApi ProviderApi);
	FUnrealAIError MakeError(const FString& Message, int32 HttpStatus = 0, const FString& RawJson = FString());
}
