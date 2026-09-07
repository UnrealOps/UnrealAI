// Copyright EngineWorks. All Rights Reserved.
#pragma once
#include "Auth/UnrealAIProviderAccess.h"

enum class EUnrealAIApiKeyProvisionKind : uint8
{
	Invalid,
	Stored,
	Deleted,
	Failed,
	Cancelled,
	TimedOut
};

/** Content-free terminal for one setup-only API-key mutation. */
struct UNREALAIACCESS_API FUnrealAIApiKeyProvisionResult final
{
	FUnrealAIRequestId RequestId;
	EUnrealAIApiKeyProvisionKind Kind = EUnrealAIApiKeyProvisionKind::Invalid;
	FUnrealAIProviderAccessError Error;

	bool ValidateShape(FString &OutError) const;
};

class UNREALAIACCESS_API IUnrealAIApiKeyProvisionSink
{
  public:
	virtual ~IUnrealAIApiKeyProvisionSink() = default;
	virtual void EnqueueApiKeyProvisionResult(FUnrealAIApiKeyProvisionResult &&Result) = 0;
};

class UNREALAIACCESS_API IUnrealAIApiKeyProvisionHandle
{
  public:
	virtual ~IUnrealAIApiKeyProvisionHandle() = default;
	virtual FUnrealAIRequestId GetRequestId() const = 0;
	virtual void Cancel() = 0;
};

/** Optional provisioning capability. The secure-store implementation is supplied by UnrealAIAuth. */
class UNREALAIACCESS_API IUnrealAIApiKeyProvisioner
{
  public:
	virtual ~IUnrealAIApiKeyProvisioner() = default;
	virtual bool StartStore(const FUnrealAIRequestId &RequestId, FUnrealAISecretValue &&ApiKey, float TimeoutSeconds,
							TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink,
							const FUnrealAICancellationToken &Cancellation,
							TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> &OutHandle,
							FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool StartDelete(const FUnrealAIRequestId &RequestId, float TimeoutSeconds,
							 TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink,
							 const FUnrealAICancellationToken &Cancellation,
							 TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> &OutHandle,
							 FUnrealAIProviderAccessError &OutError) = 0;
	virtual void BeginShutdown() = 0;
	virtual bool IsShutdown() const = 0;
	virtual int32 GetActiveOperationCount() const = 0;
};
