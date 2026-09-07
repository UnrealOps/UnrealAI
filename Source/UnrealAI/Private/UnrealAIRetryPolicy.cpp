#include "UnrealAIRetryPolicy.h"
#include "Runtime/UnrealAIFailureClassification.h"
#include "Runtime/UnrealAIProviderReliability.h"

namespace UnrealAIRetryPolicyPrivate
{
bool ContainsPermanentQuotaMarker(const FUnrealAIError &Error)
{
	const FTCHARToUTF8 Body(*Error.RawJson);
	return UE::UnrealAI::Reliability::IsPermanentQuotaCode(Error.Type) ||
		   UE::UnrealAI::Reliability::IsPermanentQuotaCode(Error.Code) ||
		   UE::UnrealAI::Reliability::IsPermanentQuotaResponse(
			   MakeArrayView(reinterpret_cast<const uint8 *>(Body.Get()), Body.Length()));
}

bool TryParseRetryAfter(const FString &RetryAfter, const FDateTime &UtcNow, double &OutDelaySeconds)
{
	const FString Trimmed = RetryAfter.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return false;
	}

	if (Trimmed.IsNumeric())
	{
		OutDelaySeconds = FMath::Max(0.0, FCString::Atod(*Trimmed));
		return true;
	}

	FDateTime RetryTime;
	if (!FDateTime::ParseHttpDate(Trimmed, RetryTime))
	{
		return false;
	}

	OutDelaySeconds = FMath::Max(0.0, (RetryTime - UtcNow).GetTotalSeconds());
	return true;
}
} // namespace UnrealAIRetryPolicyPrivate

FUnrealAIRetryPolicy UnrealAIRetryPolicy::Resolve(const FUnrealAIRetryPolicy &ProviderPolicy,
												  const FUnrealAIRequestRetryOptions &RequestOptions)
{
	FUnrealAIRetryPolicy Policy = ProviderPolicy;
	Policy.MaxRetries = FMath::Clamp(Policy.MaxRetries, 0, MaxSupportedRetries);
	Policy.InitialDelaySeconds = FMath::Clamp(Policy.InitialDelaySeconds, 0.1f, 3600.0f);
	Policy.MaxDelaySeconds = FMath::Clamp(Policy.MaxDelaySeconds, 0.1f, 3600.0f);

	switch (RequestOptions.Mode)
	{
	case EUnrealAIRetryMode::Disabled:
		Policy.MaxRetries = 0;
		break;
	case EUnrealAIRetryMode::OverrideMaxRetries:
		Policy.MaxRetries = FMath::Clamp(RequestOptions.MaxRetries, 0, MaxSupportedRetries);
		break;
	case EUnrealAIRetryMode::UseProviderPolicy:
	default:
		break;
	}
	return Policy;
}

bool UnrealAIRetryPolicy::IsRetryable(const FUnrealAIRetryPolicy &Policy, int32 RetriesAttempted,
									  const FUnrealAIRetryFailure &Failure)
{
	if (RetriesAttempted >= Policy.MaxRetries ||
		UnrealAIRetryPolicyPrivate::ContainsPermanentQuotaMarker(Failure.Error))
	{
		return false;
	}

	switch (Failure.Reason)
	{
	case EUnrealAIRetryReason::ConnectionError:
	case EUnrealAIRetryReason::Timeout:
	case EUnrealAIRetryReason::EmptyStream:
		return true;
	case EUnrealAIRetryReason::HttpError:
	default:
		return UE::UnrealAI::Reliability::IsRetryableHttpStatus(Failure.HttpStatus);
	}
}

bool UnrealAIRetryPolicy::CanRetryStream(bool bSawCompleteDataEvent)
{
	return !bSawCompleteDataEvent;
}

bool UnrealAIRetryPolicy::TryComputeDelay(const FUnrealAIRetryPolicy &Policy, int32 RetryNumber,
										  const FString &RetryAfter, const FDateTime &UtcNow, float JitterFraction,
										  float &OutDelaySeconds)
{
	if (RetryNumber <= 0 || Policy.MaxDelaySeconds <= 0.0f)
	{
		return false;
	}

	double DelaySeconds = FUnrealAIProviderRetryMath::ExponentialDelay(
		Policy.InitialDelaySeconds, Policy.MaxDelaySeconds, RetryNumber, JitterFraction, 0.25, false);

	double ServerDelaySeconds = 0.0;
	if (UnrealAIRetryPolicyPrivate::TryParseRetryAfter(RetryAfter, UtcNow, ServerDelaySeconds))
	{
		if (ServerDelaySeconds > static_cast<double>(Policy.MaxDelaySeconds))
		{
			return false;
		}
		DelaySeconds = FMath::Max(DelaySeconds, ServerDelaySeconds);
	}

	OutDelaySeconds = static_cast<float>(DelaySeconds);
	return true;
}
