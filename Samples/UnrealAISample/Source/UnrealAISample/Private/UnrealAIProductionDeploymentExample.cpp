#include "UnrealAIProductionDeploymentExample.h"

#include "UnrealAIClient.h"
#include "UnrealAIProviders.h"

UUnrealAIClient* FUnrealAIProductionDeploymentExample::CreatePackagedClientProxy(
	UObject* Outer,
	const FString& ShortLivedGameSessionToken,
	FUnrealAIError& OutError)
{
	FUnrealAIProviderConfig BackendConfig;
	BackendConfig.Name = TEXT("GameAIBackend");
	BackendConfig.Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
	BackendConfig.BaseUrl = TEXT("https://ai.example.invalid/v1");
	BackendConfig.DefaultModel = TEXT("game-chat");
	BackendConfig.bRequiresApiKey = true;
	BackendConfig.ApiKeyEnvironmentVariable.Reset();
	BackendConfig.ApiKeyOverride = ShortLivedGameSessionToken;
	BackendConfig.TimeoutSeconds = 30.0f;

	return UUnrealAIProviders::OpenAICompatible(
		Outer,
		BackendConfig,
		OutError);
}

UUnrealAIClient* FUnrealAIProductionDeploymentExample::CreateDedicatedServerProvider(
	UObject* Outer,
	FName ProviderName,
	FUnrealAIError& OutError)
{
	UUnrealAIClient* Client = NewObject<UUnrealAIClient>(Outer);
	if (!Client->ConfigureFromSettings(ProviderName, OutError))
	{
		return nullptr;
	}
	return Client;
}
