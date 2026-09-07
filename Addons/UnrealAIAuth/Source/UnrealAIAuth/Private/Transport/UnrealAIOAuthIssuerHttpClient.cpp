// Copyright UnrealOps. All Rights Reserved.

#include "Transport/UnrealAIOAuthIssuerHttpClient.h"

#include "Transport/UnrealAIMacOAuthIssuerHttpClient.h"

namespace
{
FUnrealAIProviderAccessError MakeUnavailableError()
{
	FUnrealAIProviderAccessError Error;
	Error.Category = EUnrealAIErrorCategory::UnsupportedCapability;
	Error.Code = EUnrealAIProviderAccessErrorCode::UnsupportedCapability;
	return Error;
}

class FUnavailableOAuthIssuerHttpClient final : public IUnrealAIOAuthIssuerHttpClient
{
  public:
	bool Execute(const FUnrealAIOAuthAuthorizationOperationContext &, const FUnrealAIOAuthIssuerHttpRequest &,
				 FUnrealAIOAuthIssuerHttpResponse &OutResponse, FUnrealAIProviderAccessError &OutError) override
	{
		OutResponse.Reset();
		OutError = MakeUnavailableError();
		return false;
	}
};
} // namespace

bool FUnrealAIOAuthIssuerHttpClientOptions::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!FMath::IsFinite(CancellationPollSeconds) || CancellationPollSeconds < 0.001 || CancellationPollSeconds > 0.25)
	{
		OutError = TEXT("OAuth issuer HTTP cancellation polling must remain between 1ms and 250ms.");
		return false;
	}
	return true;
}

bool IsAgentPlatformOAuthIssuerHttpClientSupported()
{
#if PLATFORM_MAC
	return true;
#else
	return false;
#endif
}

TSharedRef<IUnrealAIOAuthIssuerHttpClient, ESPMode::ThreadSafe>
CreateAgentPlatformOAuthIssuerHttpClient(const FUnrealAIOAuthIssuerHttpClientOptions &Options)
{
	FString Error;
	if (!Options.ValidateShape(Error))
	{
		return MakeShared<FUnavailableOAuthIssuerHttpClient, ESPMode::ThreadSafe>();
	}
#if PLATFORM_MAC
	return CreateAgentMacOAuthIssuerHttpClient(Options);
#else
	return MakeShared<FUnavailableOAuthIssuerHttpClient, ESPMode::ThreadSafe>();
#endif
}
