// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <atomic>

#include "UnrealAICancellation.generated.h"

/** Stable cancellation reason propagated through cooperative async work. */
UENUM(BlueprintType)
enum class EUnrealAICancellationReason : uint8
{
	None,
	Requested,
	Timeout,
	Shutdown,
	OwnerDestroyed,
	WorldInvalidated,
	BudgetExceeded,
	Superseded
};

namespace UE::UnrealAI::Private
{
class FCancellationState;
}

/** Cheap, thread-safe observation handle. It never owns a UObject. */
class UNREALAIACCESS_API FUnrealAICancellationToken final
{
  public:
	FUnrealAICancellationToken() = default;

	bool IsValid() const;
	bool IsCancellationRequested() const;
	EUnrealAICancellationReason GetReason() const;

  private:
	explicit FUnrealAICancellationToken(
		TSharedPtr<UE::UnrealAI::Private::FCancellationState, ESPMode::ThreadSafe> InState);

	TSharedPtr<UE::UnrealAI::Private::FCancellationState, ESPMode::ThreadSafe> State;

	friend class FUnrealAICancellationSource;
};

/** Cancellation owner. Repeated cancellation is idempotent and first reason wins. */
class UNREALAIACCESS_API FUnrealAICancellationSource final
{
  public:
	FUnrealAICancellationSource();
	explicit FUnrealAICancellationSource(const FUnrealAICancellationToken &Parent);

	FUnrealAICancellationToken GetToken() const;
	bool Cancel(EUnrealAICancellationReason Reason = EUnrealAICancellationReason::Requested) const;
	bool IsCancellationRequested() const;

  private:
	TSharedPtr<UE::UnrealAI::Private::FCancellationState, ESPMode::ThreadSafe> State;
};

/** Logical terminal categories used by the race-safe one-shot guard. */
UENUM()
enum class EUnrealAITerminalKind : uint8
{
	None,
	Succeeded,
	Failed,
	Cancelled,
	TimedOut,
	Suspended
};

/** Serializes competing completion/cancel/timeout attempts to exactly one winner. */
class UNREALAIACCESS_API FUnrealAITerminalGuard final
{
  public:
	bool TryComplete(EUnrealAITerminalKind Kind);
	bool IsComplete() const;
	EUnrealAITerminalKind GetTerminalKind() const;

  private:
	std::atomic<uint8> TerminalKind{static_cast<uint8>(EUnrealAITerminalKind::None)};
};
