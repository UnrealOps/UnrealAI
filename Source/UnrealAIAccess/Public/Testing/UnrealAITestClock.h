// Copyright EngineWorks. All Rights Reserved.
#pragma once
#include "Runtime/UnrealAIClock.h"
#include "Misc/ScopeLock.h"

/** Deterministic UTC/monotonic clock for offline SDK fixtures. */
class FUnrealAITestClock final : public IUnrealAIClock
{
  public:
	explicit FUnrealAITestClock(FDateTime InUtc = FDateTime(2020, 1, 1), double InMonotonicSeconds = 0.0)
		: CurrentUtc(InUtc),
		  CurrentMonotonicSeconds(FMath::IsFinite(InMonotonicSeconds) ? FMath::Max(0.0, InMonotonicSeconds) : 0.0)
	{
	}
	FDateTime UtcNow() const override
	{
		FScopeLock Lock(&Mutex);
		return CurrentUtc;
	}
	double MonotonicSeconds() const override
	{
		FScopeLock Lock(&Mutex);
		return CurrentMonotonicSeconds;
	}
	void Advance(FTimespan Delta)
	{
		const int64 Ticks = FMath::Max<int64>(0, Delta.GetTicks());
		const double Seconds = static_cast<double>(Ticks) / static_cast<double>(ETimespan::TicksPerSecond);
		FScopeLock Lock(&Mutex);
		const int64 MaximumTicks = FDateTime::MaxValue().GetTicks();
		CurrentUtc = Ticks > MaximumTicks - CurrentUtc.GetTicks() ? FDateTime::MaxValue()
																  : FDateTime(CurrentUtc.GetTicks() + Ticks);
		CurrentMonotonicSeconds = CurrentMonotonicSeconds > TNumericLimits<double>::Max() - Seconds
									  ? TNumericLimits<double>::Max()
									  : CurrentMonotonicSeconds + Seconds;
	}

  private:
	mutable FCriticalSection Mutex;
	FDateTime CurrentUtc;
	double CurrentMonotonicSeconds = 0.0;
};
