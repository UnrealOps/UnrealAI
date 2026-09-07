// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthAuthorizationIssuerHttp.h"
#include "CoreMinimal.h"

/** Bounded platform issuer-client policy. */
struct UNREALAIAUTH_API FUnrealAIOAuthIssuerHttpClientOptions final
{
	double CancellationPollSeconds = 0.01;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	/** Keeps the native task suspended after construction so physical cancellation can be tested without network I/O.
	 */
	bool bSuspendNativeTaskForTesting = false;
	TSharedPtr<TFunction<void()>, ESPMode::ThreadSafe> AfterNativeTaskCreatedReservedForTesting;
#endif

	bool ValidateShape(FString &OutError) const;
};

/** True only where the built-in redirect-rejecting issuer HTTP client is available. */
UNREALAIAUTH_API bool IsAgentPlatformOAuthIssuerHttpClientSupported();

/** Creates the platform issuer HTTP client, or a fail-closed unavailable implementation. */
UNREALAIAUTH_API TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe>
CreateAgentPlatformOAuthIssuerHttpClient(const FUnrealAIOAuthIssuerHttpClientOptions &Options = {});
