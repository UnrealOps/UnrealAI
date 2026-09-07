// Copyright UnrealOps. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"

class UNREALAIACCESS_API IUnrealAIClock
{
  public:
	virtual ~IUnrealAIClock() = default;
	virtual FDateTime UtcNow() const = 0;
	virtual double MonotonicSeconds() const = 0;
};

class UNREALAIACCESS_API FUnrealAISystemClock final : public IUnrealAIClock
{
  public:
	FDateTime UtcNow() const override;
	double MonotonicSeconds() const override;
};

/** Immutable monotonic deadline; negative duration expires immediately. */
struct UNREALAIACCESS_API FUnrealAIDeadline
{
	double AtMonotonicSeconds = 0.0;

	/** Builds a saturating deadline from one already-captured monotonic sample. */
	static FUnrealAIDeadline FromMonotonicSample(double MonotonicSeconds, double DurationSeconds);
	static FUnrealAIDeadline FromNow(const IUnrealAIClock &Clock, double DurationSeconds);
	/** Fail-closed expiry check against one already-captured monotonic sample. */
	bool IsExpiredAtMonotonicSample(double MonotonicSeconds) const;
	bool IsExpired(const IUnrealAIClock &Clock) const;
	double RemainingSeconds(const IUnrealAIClock &Clock) const;
};
