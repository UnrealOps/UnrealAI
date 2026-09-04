#pragma once

#include "CoreMinimal.h"
#include "UnrealAITypes.h"

struct FUnrealAIRetryFailure
{
	EUnrealAIRetryReason Reason = EUnrealAIRetryReason::HttpError;
	int32 HttpStatus = 0;
	FUnrealAIError Error;
	FString RetryAfter;
};

namespace UnrealAIRetryPolicy
{
	constexpr int32 MaxSupportedRetries = 10;

	FUnrealAIRetryPolicy Resolve(
		const FUnrealAIRetryPolicy& ProviderPolicy,
		const FUnrealAIRequestRetryOptions& RequestOptions);

	bool IsRetryable(
		const FUnrealAIRetryPolicy& Policy,
		int32 RetriesAttempted,
		const FUnrealAIRetryFailure& Failure);

	bool CanRetryStream(bool bSawCompleteDataEvent);

	bool TryComputeDelay(
		const FUnrealAIRetryPolicy& Policy,
		int32 RetryNumber,
		const FString& RetryAfter,
		const FDateTime& UtcNow,
		float JitterFraction,
		float& OutDelaySeconds);
}
