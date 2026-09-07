// Copyright UnrealOps. All Rights Reserved.

#include "XAI/UnrealAIXAIProviderPolicy.h"

namespace UE::UnrealAI::XAI
{
namespace
{
const FName XAIProviderPolicyModelName(TEXT("xai.responses"));
const FName XAIProviderPolicyAuthName(TEXT("xai.grok.oauth"));
const FName XAIProviderPolicyConnectionAlias(TEXT("xai.grok.subscription.default"));
} // namespace

FName GetModelProviderName()
{
	return XAIProviderPolicyModelName;
}

FName GetAccountAuthProviderName()
{
	return XAIProviderPolicyAuthName;
}

FName GetConnectionAlias()
{
	return XAIProviderPolicyConnectionAlias;
}

bool TryBuildResponsesPolicy(FUnrealAIOpenAIResponsesProviderConfig &OutConfig, FString &OutError)
{
	OutConfig = {};
	OutError.Reset();
	OutConfig.ProviderName = XAIProviderPolicyModelName;
	OutConfig.AccountAuthProviderName = XAIProviderPolicyAuthName;
	OutConfig.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	OutConfig.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	if (!FUnrealAIEndpointOrigin::TryParse(TEXT("https://api.x.ai"), false, OutConfig.EndpointOrigin, OutError))
	{
		OutConfig = {};
		return false;
	}
	OutConfig.Audience = TEXT("https://api.x.ai/v1/responses");
	OutConfig.RelativePath = TEXT("/v1/responses");
	OutConfig.CredentialPresentation = EUnrealAIHttpCredentialPresentation::AuthorizationBearer;
	OutConfig.ForbiddenErrorCode = TEXT("xai_entitlement_denied");
	OutConfig.ForbiddenErrorCategory = EUnrealAIErrorCategory::PolicyDenied;
	OutConfig.PublicFaultPolicy.CodePrefix = TEXT("xai");
	OutConfig.PublicFaultPolicy.ProviderDisplayName = TEXT("xAI");
	OutConfig.ModelProfiles.Add(OutConfig.MakeToolCapableModelProfile(TEXT("grok-4.3")));
	if (!OutConfig.ValidateShape(OutError))
	{
		OutConfig = {};
		return false;
	}
	return true;
}
} // namespace UE::UnrealAI::XAI
