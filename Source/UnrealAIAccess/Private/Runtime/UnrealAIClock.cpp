// Copyright UnrealOps. All Rights Reserved.
#include "Runtime/UnrealAIClock.h"
#include "HAL/PlatformTime.h"
#include "Math/NumericLimits.h"
FDateTime FUnrealAISystemClock::UtcNow() const
{
	return FDateTime::UtcNow();
}
double FUnrealAISystemClock::MonotonicSeconds() const
{
	return FPlatformTime::Seconds();
}

FUnrealAIDeadline FUnrealAIDeadline::FromMonotonicSample(const double MonotonicSeconds, const double DurationSeconds)
{
	FUnrealAIDeadline Result;
	if (!FMath::IsFinite(MonotonicSeconds) || MonotonicSeconds < 0.0 || !FMath::IsFinite(DurationSeconds) ||
		DurationSeconds < 0.0)
	{
		Result.AtMonotonicSeconds = 0.0;
		return Result;
	}
	Result.AtMonotonicSeconds = MonotonicSeconds > TNumericLimits<double>::Max() - DurationSeconds
									? TNumericLimits<double>::Max()
									: MonotonicSeconds + DurationSeconds;
	return Result;
}

FUnrealAIDeadline FUnrealAIDeadline::FromNow(const IUnrealAIClock &Clock, const double DurationSeconds)
{
	return FromMonotonicSample(Clock.MonotonicSeconds(), DurationSeconds);
}

bool FUnrealAIDeadline::IsExpiredAtMonotonicSample(const double MonotonicSeconds) const
{
	return !FMath::IsFinite(MonotonicSeconds) || MonotonicSeconds < 0.0 || !FMath::IsFinite(AtMonotonicSeconds) ||
		   AtMonotonicSeconds < 0.0 || MonotonicSeconds >= AtMonotonicSeconds;
}

bool FUnrealAIDeadline::IsExpired(const IUnrealAIClock &Clock) const
{
	return IsExpiredAtMonotonicSample(Clock.MonotonicSeconds());
}

double FUnrealAIDeadline::RemainingSeconds(const IUnrealAIClock &Clock) const
{
	const double Now = Clock.MonotonicSeconds();
	if (!FMath::IsFinite(Now) || Now < 0.0 || !FMath::IsFinite(AtMonotonicSeconds) || AtMonotonicSeconds < 0.0)
	{
		return 0.0;
	}
	return FMath::Max(0.0, AtMonotonicSeconds - Now);
}
