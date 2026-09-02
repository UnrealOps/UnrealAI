#pragma once

#include "CoreMinimal.h"
#include "UnrealAITypes.h"
#include "UnrealAIProviders.generated.h"

class UUnrealAIClient;

/** Native provider factories inspired by provider instances in multi-provider AI SDKs. */
UCLASS(Abstract)
class UNREALAI_API UUnrealAIProviders : public UObject
{
	GENERATED_BODY()

public:
	static UUnrealAIClient* OpenAI(UObject* Outer, FUnrealAIError& OutError, const FString& ModelOverride = FString());
	static UUnrealAIClient* XAI(UObject* Outer, FUnrealAIError& OutError, const FString& ModelOverride = FString());
	static UUnrealAIClient* Anthropic(UObject* Outer, FUnrealAIError& OutError, const FString& ModelOverride = FString());
	static UUnrealAIClient* Gemini(UObject* Outer, FUnrealAIError& OutError, const FString& ModelOverride = FString());

	static UUnrealAIClient* OpenAICompatibleFromProfile(
		UObject* Outer,
		FName ProfileName,
		FUnrealAIError& OutError,
		const FString& ModelOverride = FString());

	static UUnrealAIClient* OpenAICompatible(
		UObject* Outer,
		const FUnrealAIProviderConfig& ProviderConfig,
		FUnrealAIError& OutError,
		const FString& ModelOverride = FString());

private:
	static UUnrealAIClient* CreateFromProfile(
		UObject* Outer,
		FName ProfileName,
		EUnrealAIProviderApi ExpectedApi,
		FUnrealAIError& OutError,
		const FString& ModelOverride);

	static UUnrealAIClient* CreateConfiguredClient(
		UObject* Outer,
		const FUnrealAIProviderConfig& ProviderConfig,
		FUnrealAIError& OutError,
		const FString& ModelOverride);
};
