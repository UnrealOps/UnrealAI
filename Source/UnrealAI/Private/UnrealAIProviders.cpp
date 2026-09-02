#include "UnrealAIProviders.h"

#include "UnrealAIClient.h"
#include "UnrealAISettings.h"

namespace UnrealAIProvidersPrivate
{
	FUnrealAIError MakeError(const FString& Message)
	{
		FUnrealAIError Error;
		Error.bIsError = true;
		Error.Message = Message;
		return Error;
	}
}

UUnrealAIClient* UUnrealAIProviders::OpenAI(UObject* Outer, FUnrealAIError& OutError, const FString& ModelOverride)
{
	return CreateFromProfile(
		Outer,
		TEXT("OpenAI"),
		EUnrealAIProviderApi::OpenAICompatibleChatCompletions,
		OutError,
		ModelOverride);
}

UUnrealAIClient* UUnrealAIProviders::XAI(UObject* Outer, FUnrealAIError& OutError, const FString& ModelOverride)
{
	return CreateFromProfile(
		Outer,
		TEXT("XAI"),
		EUnrealAIProviderApi::OpenAICompatibleChatCompletions,
		OutError,
		ModelOverride);
}

UUnrealAIClient* UUnrealAIProviders::Anthropic(UObject* Outer, FUnrealAIError& OutError, const FString& ModelOverride)
{
	return CreateFromProfile(
		Outer,
		TEXT("Anthropic"),
		EUnrealAIProviderApi::AnthropicMessages,
		OutError,
		ModelOverride);
}

UUnrealAIClient* UUnrealAIProviders::Gemini(UObject* Outer, FUnrealAIError& OutError, const FString& ModelOverride)
{
	return CreateFromProfile(
		Outer,
		TEXT("Gemini"),
		EUnrealAIProviderApi::GeminiGenerateContent,
		OutError,
		ModelOverride);
}

UUnrealAIClient* UUnrealAIProviders::OpenAICompatibleFromProfile(
	UObject* Outer,
	FName ProfileName,
	FUnrealAIError& OutError,
	const FString& ModelOverride)
{
	return CreateFromProfile(
		Outer,
		ProfileName,
		EUnrealAIProviderApi::OpenAICompatibleChatCompletions,
		OutError,
		ModelOverride);
}

UUnrealAIClient* UUnrealAIProviders::OpenAICompatible(
	UObject* Outer,
	const FUnrealAIProviderConfig& ProviderConfig,
	FUnrealAIError& OutError,
	const FString& ModelOverride)
{
	FUnrealAIProviderConfig CompatibleConfig = ProviderConfig;
	CompatibleConfig.Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
	return CreateConfiguredClient(Outer, CompatibleConfig, OutError, ModelOverride);
}

UUnrealAIClient* UUnrealAIProviders::CreateFromProfile(
	UObject* Outer,
	FName ProfileName,
	EUnrealAIProviderApi ExpectedApi,
	FUnrealAIError& OutError,
	const FString& ModelOverride)
{
	const UUnrealAISettings* Settings = GetDefault<UUnrealAISettings>();
	if (!Settings)
	{
		OutError = UnrealAIProvidersPrivate::MakeError(TEXT("UnrealAI settings are unavailable."));
		return nullptr;
	}

	FUnrealAIProviderConfig ProviderConfig;
	if (!Settings->TryGetProviderConfig(ProfileName, ProviderConfig))
	{
		OutError = UnrealAIProvidersPrivate::MakeError(
			FString::Printf(TEXT("Provider profile '%s' was not found."), *ProfileName.ToString()));
		return nullptr;
	}

	if (ProviderConfig.Api != ExpectedApi)
	{
		OutError = UnrealAIProvidersPrivate::MakeError(
			FString::Printf(TEXT("Provider profile '%s' uses an incompatible API protocol."), *ProfileName.ToString()));
		return nullptr;
	}

	return CreateConfiguredClient(Outer, ProviderConfig, OutError, ModelOverride);
}

UUnrealAIClient* UUnrealAIProviders::CreateConfiguredClient(
	UObject* Outer,
	const FUnrealAIProviderConfig& ProviderConfig,
	FUnrealAIError& OutError,
	const FString& ModelOverride)
{
	if (!IsValid(Outer))
	{
		OutError = UnrealAIProvidersPrivate::MakeError(TEXT("A valid UObject outer is required to create an UnrealAI client."));
		return nullptr;
	}

	FUnrealAIProviderConfig ResolvedConfig = ProviderConfig;
	if (!ModelOverride.IsEmpty())
	{
		ResolvedConfig.DefaultModel = ModelOverride;
	}

	if (ResolvedConfig.BaseUrl.TrimStartAndEnd().IsEmpty())
	{
		OutError = UnrealAIProvidersPrivate::MakeError(TEXT("The provider base URL is empty."));
		return nullptr;
	}

	UUnrealAIClient* Client = NewObject<UUnrealAIClient>(Outer);
	if (!Client)
	{
		OutError = UnrealAIProvidersPrivate::MakeError(TEXT("Failed to create an UnrealAI client."));
		return nullptr;
	}

	Client->Configure(ResolvedConfig);
	OutError = FUnrealAIError();
	return Client;
}
