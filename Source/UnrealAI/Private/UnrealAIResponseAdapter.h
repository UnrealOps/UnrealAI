#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UnrealAIProviderAdapter.h"
#include "UnrealAIResponseTypes.h"

struct FUnrealAIResponseContext
{
	EUnrealAIProviderApi Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
	bool bNativeResponses = false;
	bool bStore = false;
	FString Model;
	FString Binding;
	FString InputJson;
};

struct FUnrealAIResponseState
{
	FUnrealAIResponseContext Context;
	FUnrealAIResponseResult Result;
	TSharedPtr<FJsonObject> Snapshot;
	TMap<int32, FString> ArgumentBuffers;
	TSet<int32> FinishedItems;
	TMap<int32, int32> ArgumentByteCounts;
	TMap<int32, FString> StableItemIds;
	bool bSawTerminal = false;
	bool bSawData = false;
};

namespace UnrealAIResponseAdapters
{
	constexpr int32 MaxToolArgumentBytes = 1024 * 1024;
	FUnrealAIResponseCapabilities Capabilities(const FUnrealAIProviderConfig& Config);
	bool BuildRequest(const FUnrealAIProviderConfig& Config, const FUnrealAIResponseRequest& Request,
		const FString& ApiKey, EUnrealAIRequestMode Mode, FUnrealAIHttpRequestData& OutHttp,
		FUnrealAIResponseContext& OutContext, FUnrealAIError& OutError);
	void ParseResponse(int32 HttpStatus, const FString& Json, FUnrealAIResponseState& State);
	bool ParseStreamEvent(const FUnrealAISseEvent& Frame, FUnrealAIResponseState& State,
		TArray<FUnrealAIResponseEvent>& OutEvents, FUnrealAIError& OutError);
	bool CanCompleteStream(const FUnrealAIResponseState& State);
	void Normalize(FUnrealAIResponseState& State, bool bComplete);
	void BuildContinuation(FUnrealAIResponseState& State);
}
