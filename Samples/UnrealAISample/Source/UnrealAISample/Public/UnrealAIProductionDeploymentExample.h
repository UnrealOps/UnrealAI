#pragma once

#include "CoreMinimal.h"
#include "UnrealAITypes.h"

class UUnrealAIClient;

/** Compile-tested configuration boundary used by the production deployment skill recipes. */
class UNREALAISAMPLE_API FUnrealAIProductionDeploymentExample
{
public:
	/** Creates a client for a trusted OpenAI-compatible game backend. */
	static UUnrealAIClient* CreatePackagedClientProxy(
		UObject* Outer,
		const FString& ShortLivedGameSessionToken,
		FUnrealAIError& OutError);

	/** Creates a direct provider client for a controlled dedicated-server process. */
	static UUnrealAIClient* CreateDedicatedServerProvider(
		UObject* Outer,
		FName ProviderName,
		FUnrealAIError& OutError);
};
