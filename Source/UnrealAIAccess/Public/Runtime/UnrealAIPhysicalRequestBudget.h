// Copyright UnrealOps. All Rights Reserved.
#pragma once
#include "UnrealAIAccessTypes.h"
#include <atomic>

class FUnrealAIPhysicalRequestBudget;

/** One physical-admission reservation. Logical cancellation alone never releases it. */
class UNREALAIACCESS_API FUnrealAIPhysicalRequestPermit final
{
  public:
	FUnrealAIPhysicalRequestPermit(TSharedRef<FUnrealAIPhysicalRequestBudget, ESPMode::ThreadSafe> InBudget,
								   FGuid InId);
	~FUnrealAIPhysicalRequestPermit();
	void Release();

  private:
	TSharedRef<FUnrealAIPhysicalRequestBudget, ESPMode::ThreadSafe> Budget;
	FGuid Id;
	std::atomic<bool> bReleased{false};
};

/** Shared bounded admission across a service's connections and model profiles. */
class UNREALAIACCESS_API FUnrealAIPhysicalRequestBudget final
	: public TSharedFromThis<FUnrealAIPhysicalRequestBudget, ESPMode::ThreadSafe>
{
  public:
	explicit FUnrealAIPhysicalRequestBudget(int32 InMaximumRequests = 32);
	TSharedPtr<FUnrealAIPhysicalRequestPermit, ESPMode::ThreadSafe> TryAcquire(const FUnrealAIRequestId &RequestId);
	int32 Num() const;

  private:
	friend class FUnrealAIPhysicalRequestPermit;
	void Release(FGuid RequestId);
	mutable FCriticalSection Mutex;
	TSet<FGuid> Requests;
	int32 MaximumRequests;
};
