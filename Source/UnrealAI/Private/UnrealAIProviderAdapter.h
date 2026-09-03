#pragma once

#include "CoreMinimal.h"
#include "UnrealAISseParser.h"
#include "UnrealAITypes.h"

enum class EUnrealAIRequestMode : uint8
{
	OneShot,
	Stream
};

struct FUnrealAIHttpRequestData
{
	FString Url;
	FString Verb = TEXT("POST");
	TMap<FString, FString> Headers;
	FString Body;
	FString ResolvedModel;
};

struct FUnrealAIProviderStreamState
{
	FUnrealAIChatResponse Response;
	int32 ExpectedChoiceCount = 1;
	bool bSawDataEvent = false;
	bool bSawTerminalEvent = false;
	bool bSawFinishReason = false;
};

class IUnrealAIProviderAdapter
{
public:
	virtual ~IUnrealAIProviderAdapter() = default;

	virtual bool BuildRequest(
		const FUnrealAIProviderConfig& ProviderConfig,
		const FUnrealAIChatRequest& Request,
		const FString& ApiKey,
		EUnrealAIRequestMode RequestMode,
		FUnrealAIHttpRequestData& OutRequest,
		FUnrealAIError& OutError) const = 0;

	virtual void ParseResponse(
		const FString& ResolvedModel,
		int32 HttpStatus,
		const FString& RawJson,
		FUnrealAIChatResponse& OutResponse,
		FUnrealAIError& OutError) const = 0;

	virtual bool ParseStreamEvent(
		const FUnrealAISseEvent& SseEvent,
		FUnrealAIProviderStreamState& State,
		TArray<FUnrealAIChatStreamEvent>& OutEvents,
		FUnrealAIError& OutError) const = 0;

	virtual bool CanCompleteStream(const FUnrealAIProviderStreamState& State) const = 0;
};

namespace UnrealAIProviderAdapters
{
	const IUnrealAIProviderAdapter& Get(EUnrealAIProviderApi ProviderApi);
	FUnrealAIError MakeError(const FString& Message, int32 HttpStatus = 0, const FString& RawJson = FString());
}
