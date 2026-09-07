// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OpenAI/UnrealAIOpenAIResponsesProvider.h"

namespace UE::UnrealAI::OpenAICodex
{
UNREALAIAUTHOPENAI_API FName GetModelProviderName();
UNREALAIAUTHOPENAI_API FName GetAccountAuthProviderName();
UNREALAIAUTHOPENAI_API FName GetConnectionAlias();

/**
 * Produces the single reviewed ChatGPT Codex Responses resource policy.
 *
 * The function has no caller-provided endpoint, path, presentation, or header values. Compatibility-policy changes
 * therefore require a source change and deterministic fixture review.
 */
UNREALAIAUTHOPENAI_API bool TryBuildResponsesPolicy(FUnrealAIOpenAIResponsesProviderConfig &OutConfig,
													FString &OutError);
} // namespace UE::UnrealAI::OpenAICodex
