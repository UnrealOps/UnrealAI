// Copyright UnrealOps. All Rights Reserved.

#include "Runtime/UnrealAICancellation.h"

namespace UE::UnrealAI::Private
{
bool IsKnownCancellationReason(const EUnrealAICancellationReason Reason)
{
	switch (Reason)
	{
	case EUnrealAICancellationReason::Requested:
	case EUnrealAICancellationReason::Timeout:
	case EUnrealAICancellationReason::Shutdown:
	case EUnrealAICancellationReason::OwnerDestroyed:
	case EUnrealAICancellationReason::WorldInvalidated:
	case EUnrealAICancellationReason::BudgetExceeded:
	case EUnrealAICancellationReason::Superseded:
		return true;
	case EUnrealAICancellationReason::None:
	default:
		return false;
	}
}

bool IsKnownTerminalKind(const EUnrealAITerminalKind Kind)
{
	switch (Kind)
	{
	case EUnrealAITerminalKind::Succeeded:
	case EUnrealAITerminalKind::Failed:
	case EUnrealAITerminalKind::Cancelled:
	case EUnrealAITerminalKind::TimedOut:
	case EUnrealAITerminalKind::Suspended:
		return true;
	case EUnrealAITerminalKind::None:
	default:
		return false;
	}
}

class FCancellationState final
{
  public:
	static constexpr int32 MaxParentDepth = 64;

	explicit FCancellationState(const TSharedPtr<FCancellationState, ESPMode::ThreadSafe> &InParent = nullptr)
	{
		if (InParent.IsValid() && InParent->Depth >= MaxParentDepth)
		{
			// Fail closed instead of creating an unbounded recursive chain.
			Depth = MaxParentDepth;
			Reason.store(static_cast<uint8>(EUnrealAICancellationReason::Requested), std::memory_order_relaxed);
		}
		else
		{
			Parent = InParent;
			Depth = InParent.IsValid() ? InParent->Depth + 1 : 0;
		}
	}

	bool Cancel(EUnrealAICancellationReason InReason)
	{
		const EUnrealAICancellationReason NormalizedReason =
			IsKnownCancellationReason(InReason) ? InReason : EUnrealAICancellationReason::Requested;
		uint8 Expected = static_cast<uint8>(EUnrealAICancellationReason::None);
		return Reason.compare_exchange_strong(Expected, static_cast<uint8>(NormalizedReason), std::memory_order_acq_rel,
											  std::memory_order_acquire);
	}

	bool Query(EUnrealAICancellationReason &OutReason) const
	{
		const uint8 RawReason = Reason.load(std::memory_order_acquire);
		if (RawReason != static_cast<uint8>(EUnrealAICancellationReason::None))
		{
			OutReason = static_cast<EUnrealAICancellationReason>(RawReason);
			return true;
		}
		return Parent.IsValid() && Parent->Query(OutReason);
	}

  private:
	// A child retains its parent so cancellation remains observable even if
	// the source and all other parent tokens have already been destroyed.
	TSharedPtr<FCancellationState, ESPMode::ThreadSafe> Parent;
	int32 Depth = 0;
	std::atomic<uint8> Reason{static_cast<uint8>(EUnrealAICancellationReason::None)};
};
} // namespace UE::UnrealAI::Private

FUnrealAICancellationToken::FUnrealAICancellationToken(
	TSharedPtr<UE::UnrealAI::Private::FCancellationState, ESPMode::ThreadSafe> InState)
	: State(MoveTemp(InState))
{
}

bool FUnrealAICancellationToken::IsValid() const
{
	return State.IsValid();
}

bool FUnrealAICancellationToken::IsCancellationRequested() const
{
	EUnrealAICancellationReason Unused = EUnrealAICancellationReason::None;
	return State.IsValid() && State->Query(Unused);
}

EUnrealAICancellationReason FUnrealAICancellationToken::GetReason() const
{
	EUnrealAICancellationReason Reason = EUnrealAICancellationReason::None;
	if (State.IsValid())
	{
		State->Query(Reason);
	}
	return Reason;
}

FUnrealAICancellationSource::FUnrealAICancellationSource()
	: State(MakeShared<UE::UnrealAI::Private::FCancellationState, ESPMode::ThreadSafe>())
{
}

FUnrealAICancellationSource::FUnrealAICancellationSource(const FUnrealAICancellationToken &Parent)
	: State(MakeShared<UE::UnrealAI::Private::FCancellationState, ESPMode::ThreadSafe>(Parent.State))
{
}

FUnrealAICancellationToken FUnrealAICancellationSource::GetToken() const
{
	return FUnrealAICancellationToken(State);
}

bool FUnrealAICancellationSource::Cancel(const EUnrealAICancellationReason Reason) const
{
	return State.IsValid() && State->Cancel(Reason);
}

bool FUnrealAICancellationSource::IsCancellationRequested() const
{
	return GetToken().IsCancellationRequested();
}

bool FUnrealAITerminalGuard::TryComplete(const EUnrealAITerminalKind Kind)
{
	if (!UE::UnrealAI::Private::IsKnownTerminalKind(Kind))
	{
		return false;
	}
	uint8 Expected = static_cast<uint8>(EUnrealAITerminalKind::None);
	return TerminalKind.compare_exchange_strong(Expected, static_cast<uint8>(Kind), std::memory_order_acq_rel,
												std::memory_order_acquire);
}

bool FUnrealAITerminalGuard::IsComplete() const
{
	return TerminalKind.load(std::memory_order_acquire) != static_cast<uint8>(EUnrealAITerminalKind::None);
}

EUnrealAITerminalKind FUnrealAITerminalGuard::GetTerminalKind() const
{
	return static_cast<EUnrealAITerminalKind>(TerminalKind.load(std::memory_order_acquire));
}
