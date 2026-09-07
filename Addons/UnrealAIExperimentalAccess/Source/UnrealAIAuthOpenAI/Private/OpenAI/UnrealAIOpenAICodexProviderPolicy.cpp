// Copyright EngineWorks. All Rights Reserved.

#include "OpenAI/UnrealAIOpenAICodexProviderPolicy.h"

namespace UE::UnrealAI::OpenAICodex
{
namespace
{
const FName ModelProviderName(TEXT("openai.codex.responses"));
const FName AccountAuthProviderName(TEXT("openai.codex.oauth"));
const FName ConnectionAlias(TEXT("openai.codex.subscription.default"));
} // namespace

FName GetModelProviderName()
{
	return ModelProviderName;
}

FName GetAccountAuthProviderName()
{
	return AccountAuthProviderName;
}

FName GetConnectionAlias()
{
	return ConnectionAlias;
}

bool TryBuildResponsesPolicy(FUnrealAIOpenAIResponsesProviderConfig &OutConfig, FString &OutError)
{
	OutConfig = {};
	OutError.Reset();

	OutConfig.ProviderName = ModelProviderName;
	OutConfig.AccountAuthProviderName = AccountAuthProviderName;
	OutConfig.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	OutConfig.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	if (!FUnrealAIEndpointOrigin::TryParse(TEXT("https://chatgpt.com"), false, OutConfig.EndpointOrigin, OutError))
	{
		OutConfig = {};
		return false;
	}
	OutConfig.Audience = TEXT("https://chatgpt.com/backend-api/codex/responses");
	OutConfig.RelativePath = TEXT("/backend-api/codex/responses");
	OutConfig.CredentialPresentation = EUnrealAIHttpCredentialPresentation::AuthorizationBearerWithProtectedSecondary;
	OutConfig.ProtectedSecondaryHeaderName = TEXT("ChatGPT-Account-ID");
	OutConfig.FixedHeaders.Add({TEXT("originator"), TEXT("codex_cli_rs")});
	OutConfig.FixedHeaders.Add({TEXT("User-Agent"), TEXT("codex_cli_rs/0.0.0 (AutonomousAgents)")});
	OutConfig.ModelProfiles.Add(OutConfig.MakeToolCapableModelProfile(TEXT("gpt-5.6")));

	if (!OutConfig.ValidateShape(OutError))
	{
		OutConfig = {};
		return false;
	}
	return true;
}
} // namespace UE::UnrealAI::OpenAICodex
