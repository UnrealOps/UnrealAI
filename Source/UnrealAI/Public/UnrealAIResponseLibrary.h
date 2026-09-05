#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealAIResponseTypes.h"
#include "UnrealAIResponseLibrary.generated.h"

UCLASS()
class UNREALAI_API UUnrealAIResponseLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "UnrealAI|Responses")
	static FUnrealAIResponseItem MakeResponseMessage(const FString& Text, EUnrealAIMessageRole Role = EUnrealAIMessageRole::User);

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Responses")
	static FUnrealAIResponseRequest MakeResponseRequest(const FString& Prompt);

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Responses")
	static FUnrealAIToolDefinition MakeToolDefinition(const FString& Name, const FString& Description,
		const FString& ParametersJson, bool bStrict = false);

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Responses")
	static FString GetResponseText(const FUnrealAIResponse& Response);

	/** Only complete, valid tool calls from a successful completed turn are returned. */
	UFUNCTION(BlueprintPure, Category = "UnrealAI|Responses")
	static TArray<FUnrealAIResponseItem> GetResponseToolCalls(const FUnrealAIResponseResult& Result);

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Responses")
	static FUnrealAIResponseItem MakeToolResult(const FUnrealAIResponseItem& Call, const FString& Output, bool bIsError = false);

	/** Builds a new request without executing tools or issuing HTTP. Return all pending tool results together. */
	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Responses")
	static bool BuildContinuationRequest(const FUnrealAIResponseRequest& PreviousRequest,
		const FUnrealAIResponseResult& PreviousResult, const TArray<FUnrealAIResponseItem>& NewInput,
		FUnrealAIResponseRequest& OutRequest, FUnrealAIError& OutError);
};
