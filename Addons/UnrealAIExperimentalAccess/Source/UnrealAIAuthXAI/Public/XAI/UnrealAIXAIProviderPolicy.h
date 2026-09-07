// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OpenAI/UnrealAIOpenAIResponsesProvider.h"

namespace UE::UnrealAI::XAI
{
UNREALAIAUTHXAI_API FName GetModelProviderName();
UNREALAIAUTHXAI_API FName GetAccountAuthProviderName();
UNREALAIAUTHXAI_API FName GetConnectionAlias();
UNREALAIAUTHXAI_API bool TryBuildResponsesPolicy(FUnrealAIOpenAIResponsesProviderConfig &OutConfig, FString &OutError);
} // namespace UE::UnrealAI::XAI
