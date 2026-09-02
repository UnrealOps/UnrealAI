#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UnrealAIClient.h"
#include "UnrealAIChatComponent.generated.h"

UCLASS(ClassGroup = (AI), meta = (BlueprintSpawnableComponent))
class UNREALAI_API UUnrealAIChatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UUnrealAIChatComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI")
	FName ProviderName = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI")
	FString Model;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI", meta = (MultiLine = true))
	FString SystemPrompt;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Sampling")
	bool bUseTemperature = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Sampling", meta = (EditCondition = "bUseTemperature", ClampMin = "0.0", ClampMax = "2.0"))
	float Temperature = 1.0f;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin OnChatCompleted;

	UPROPERTY(BlueprintAssignable, Category = "UnrealAI")
	FUnrealAIChatCompletionPin OnChatFailed;

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat")
	void SendPrompt(const FString& Prompt);

	UFUNCTION(BlueprintCallable, Category = "UnrealAI|Chat")
	void SendMessages(const TArray<FUnrealAIChatMessage>& Messages);

private:
	UPROPERTY()
	TObjectPtr<UUnrealAIClient> Client;

	bool EnsureClient(FUnrealAIError& OutError);
	void HandleCompletion(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error);
};
