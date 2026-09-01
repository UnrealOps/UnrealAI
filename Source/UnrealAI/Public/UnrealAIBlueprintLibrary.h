#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealAITypes.h"
#include "UnrealAIBlueprintLibrary.generated.h"

UCLASS()
class UNREALAI_API UUnrealAIBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "UnrealAI|Chat")
	static FUnrealAIChatMessage MakeChatMessage(EUnrealAIMessageRole Role, const FString& Content);

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Chat")
	static FUnrealAIChatRequest MakeSimpleChatRequest(const FString& Prompt, const FString& Model = TEXT(""));

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Chat")
	static FString MakeJsonObjectResponseFormat();

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Chat")
	static FString MakeStrictJsonSchemaResponseFormat(const FString& SchemaName, const FString& SchemaJson, bool bStrict = true);

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Chat")
	static FString GetFirstChoiceContent(const FUnrealAIChatResponse& Response, bool& bHasContent);

	UFUNCTION(BlueprintPure, Category = "UnrealAI|Providers")
	static bool ResolveProviderConfig(FName ProviderName, FUnrealAIProviderConfig& ProviderConfig, FUnrealAIError& Error);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Environment")
	static bool ReloadProjectEnvFile(int32& VariablesLoaded, FString& StatusMessage);
};
