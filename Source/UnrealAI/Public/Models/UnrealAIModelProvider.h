// Copyright UnrealOps. All Rights Reserved.
#pragma once
#include "Models/UnrealAIModelTypes.h"
#include "Runtime/UnrealAICancellation.h"

/** Any-thread enqueue-only consumer; implementations must be bounded and nonblocking. */
class UNREALAI_API IUnrealAIModelEventSink
{
  public:
	virtual ~IUnrealAIModelEventSink() = default;
	virtual void EnqueueModelEvent(FUnrealAIModelEvent &&Event) = 0;
	/** Physical callback capability has closed. The sink is still retained through delivery and destructor tails. */
	virtual void OnPhysicalSettled() {}
};

class UNREALAI_API IUnrealAIModelRequestHandle
{
  public:
	virtual ~IUnrealAIModelRequestHandle() = default;
	virtual FUnrealAIRequestId GetRequestId() const = 0;
	virtual void Cancel() = 0;
	virtual bool IsLogicallyComplete() const = 0;
	virtual bool IsPhysicallySettled() const = 0;
};

/** One physically admitted provider turn; no world, tool execution, budget, or autonomous loop. */
class UNREALAI_API IUnrealAIModelProvider
{
  public:
	virtual ~IUnrealAIModelProvider() = default;
	virtual FName GetProviderName() const = 0;
	virtual FUnrealAIModelProviderDescriptor Describe() const = 0;
	virtual EUnrealAIModelCapability GetCapabilities(const FString &ModelId) const = 0;
	virtual FUnrealAIModelProfileProjection GetModelProjection(const FString &ModelId) const = 0;
	virtual bool StartRequest(const FUnrealAIModelRequest &Request,
							  TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext,
							  TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> Sink,
							  const FUnrealAICancellationToken &Cancellation,
							  TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> &OutHandle,
							  FUnrealAIModelError &OutError) = 0;
	virtual void BeginShutdown() = 0;
};
