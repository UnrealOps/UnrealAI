// Copyright EngineWorks. All Rights Reserved.
#include "Runtime/UnrealAIPhysicalRequestBudget.h"
#include "Misc/ScopeLock.h"

FUnrealAIPhysicalRequestPermit::FUnrealAIPhysicalRequestPermit(
	TSharedRef<FUnrealAIPhysicalRequestBudget, ESPMode::ThreadSafe> InBudget, FGuid InId)
	: Budget(MoveTemp(InBudget)), Id(InId)
{
}
FUnrealAIPhysicalRequestPermit::~FUnrealAIPhysicalRequestPermit()
{
	Release();
}
void FUnrealAIPhysicalRequestPermit::Release()
{
	if (!bReleased.exchange(true, std::memory_order_acq_rel))
	{
		Budget->Release(Id);
	}
}
FUnrealAIPhysicalRequestBudget::FUnrealAIPhysicalRequestBudget(int32 InMaximumRequests)
	: MaximumRequests(FMath::Clamp(InMaximumRequests, 1, 256))
{
}
TSharedPtr<FUnrealAIPhysicalRequestPermit, ESPMode::ThreadSafe>
FUnrealAIPhysicalRequestBudget::TryAcquire(const FUnrealAIRequestId &RequestId)
{
	FScopeLock Lock(&Mutex);
	if (!RequestId.IsValid() || Requests.Num() >= MaximumRequests)
	{
		return nullptr;
	}
	// A new physical attempt may share a logically cancelled request ID. Its permit remains independent.
	const FGuid AttemptId = FGuid::NewGuid();
	Requests.Add(AttemptId);
	return MakeShared<FUnrealAIPhysicalRequestPermit, ESPMode::ThreadSafe>(AsShared(), AttemptId);
}
void FUnrealAIPhysicalRequestBudget::Release(FGuid RequestId)
{
	FScopeLock Lock(&Mutex);
	Requests.Remove(RequestId);
}
int32 FUnrealAIPhysicalRequestBudget::Num() const
{
	FScopeLock Lock(&Mutex);
	return Requests.Num();
}
