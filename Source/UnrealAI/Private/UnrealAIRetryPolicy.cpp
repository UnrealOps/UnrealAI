#include "UnrealAIRetryPolicy.h"

namespace UnrealAIRetryPolicyPrivate
{
	bool ContainsPermanentQuotaMarker(const FUnrealAIError& Error)
	{
		const FString Values[] = {
			Error.Type,
			Error.Code,
			Error.RawJson};
		const TCHAR* Markers[] = {
			TEXT("insufficient_quota"),
			TEXT("billing_hard_limit_reached"),
			TEXT("enforced_spend_limit_reached"),
			TEXT("credit_balance_too_low")};

		for (const FString& Value : Values)
		{
			for (const TCHAR* Marker : Markers)
			{
				if (Value.Contains(Marker, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}

	bool IsRetryableHttpStatus(int32 HttpStatus)
	{
		switch (HttpStatus)
		{
		case 408:
		case 409:
		case 429:
		case 500:
		case 502:
		case 503:
		case 504:
		case 529:
			return true;
		default:
			return false;
		}
	}

	bool TryParseRetryAfter(const FString& RetryAfter, const FDateTime& UtcNow, double& OutDelaySeconds)
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
}

FUnrealAIRetryPolicy UnrealAIRetryPolicy::Resolve(
	const FUnrealAIRetryPolicy& ProviderPolicy,
	const FUnrealAIRequestRetryOptions& RequestOptions)
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

bool UnrealAIRetryPolicy::IsRetryable(
	const FUnrealAIRetryPolicy& Policy,
	int32 RetriesAttempted,
	const FUnrealAIRetryFailure& Failure)
{
	if (RetriesAttempted >= Policy.MaxRetries
		|| UnrealAIRetryPolicyPrivate::ContainsPermanentQuotaMarker(Failure.Error))
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
		return UnrealAIRetryPolicyPrivate::IsRetryableHttpStatus(Failure.HttpStatus);
	}
}

bool UnrealAIRetryPolicy::CanRetryStream(bool bSawCompleteDataEvent)
{
	return !bSawCompleteDataEvent;
}

bool UnrealAIRetryPolicy::TryComputeDelay(
	const FUnrealAIRetryPolicy& Policy,
	int32 RetryNumber,
	const FString& RetryAfter,
	const FDateTime& UtcNow,
	float JitterFraction,
	float& OutDelaySeconds)
{
	if (RetryNumber <= 0 || Policy.MaxDelaySeconds <= 0.0f)
	{
		return false;
	}

	const double ExponentialDelay = static_cast<double>(Policy.InitialDelaySeconds)
		* FMath::Pow(2.0, static_cast<double>(RetryNumber - 1));
	const double CappedBaseDelay = FMath::Min(ExponentialDelay, static_cast<double>(Policy.MaxDelaySeconds));
	const double Jitter = CappedBaseDelay * 0.25 * FMath::Clamp(static_cast<double>(JitterFraction), 0.0, 1.0);
	double DelaySeconds = FMath::Min(CappedBaseDelay + Jitter, static_cast<double>(Policy.MaxDelaySeconds));

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
