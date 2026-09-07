// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIConnectionRegistry.h"
#include "CoreMinimal.h"
#include "Runtime/UnrealAIPhysicalRequestBudget.h"
#include "Models/UnrealAIModelProvider.h"
#include "Transport/UnrealAIHttpTransport.h"

/** Provider-owned staged continuation consume committed only after physical HTTP admission. */
class UNREALAI_API IUnrealAINativeSseContinuationCommit
{
  public:
	virtual ~IUnrealAINativeSseContinuationCommit() = default;
	virtual bool IsRequired() const = 0;
	virtual bool TryCommit(FString &OutError) = 0;
};

/** Provider-owned incremental decoder behind the shared lifecycle-managed HTTPS operation. */
class UNREALAI_API IUnrealAINativeSseDecoder
{
  public:
	virtual ~IUnrealAINativeSseDecoder() = default;
	virtual bool PushBytes(TConstArrayView<uint8> Bytes, FString &OutError) = 0;
	virtual bool Finish(FString &OutError) = 0;
	virtual void Cancel() = 0;
	virtual bool IsTerminal() const = 0;
};

/**
 * Provider-module protocol strategy used by the generic SSE execution shell.
 *
 * The strategy receives only normalized request data and the already-frozen
 * credential destination. It never receives credential bytes or an arbitrary
 * URL. Image bytes are already owned and bounded before protocol lowering.
 */
class UNREALAI_API IUnrealAINativeSseProtocol
{
  public:
	using FEventSink = TFunction<void(FUnrealAIModelEvent &&)>;
	using FIgnoredEventObserver = TFunction<void()>;

	virtual ~IUnrealAINativeSseProtocol() = default;
	virtual FName GetProviderName() const = 0;
	virtual int32 GetMaximumRequestBodyBytes() const = 0;
	virtual int64 GetMaximumStreamBytes() const = 0;
	virtual bool StageRequest(const FUnrealAIModelRequest &Request, const FUnrealAICredentialDestination &Destination,
							  TArray<uint8> &OutBodyUtf8,
							  TUniquePtr<IUnrealAINativeSseContinuationCommit> &OutContinuationCommit,
							  FUnrealAIModelError &OutError) const = 0;
	virtual TUniquePtr<IUnrealAINativeSseDecoder> CreateDecoder(const FUnrealAIModelRequest &Request, FEventSink Sink,
																FIgnoredEventObserver IgnoredEventObserver) const = 0;
};

/** Exact non-secret policy for one native SSE provider registration. */
struct UNREALAI_API FUnrealAINativeSseModelProviderConfig final
{
	static constexpr int32 MaxFixedHeaders = 8;
	static constexpr int32 MaxModelProfiles = 32;
	static constexpr int32 MaxActiveRequestsLimit = 256;

	TSharedPtr<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FName ProviderName;
	FUnrealAIConnectionDescriptor Connection;
	FString RelativePath;
	EUnrealAIHttpCredentialPresentation CredentialPresentation = EUnrealAIHttpCredentialPresentation::Invalid;
	EUnrealAIHttpQueryProfile QueryProfile = EUnrealAIHttpQueryProfile::None;
	FString ProtectedSecondaryHeaderName;
	FName UnauthorizedErrorCode;
	FName ForbiddenErrorCode;
	TFunction<void(FUnrealAIModelError &)> MapPublicError;
	int32 MaximumPendingHttpEvents = 1024;
	TSharedPtr<FUnrealAIPhysicalRequestBudget, ESPMode::ThreadSafe> PhysicalBudget;
	TArray<FUnrealAIHttpRequestHeader> FixedHeaders;
	TArray<FUnrealAIModelProfileProjection> ModelProfiles;
	TSharedPtr<const IUnrealAINativeSseProtocol, ESPMode::ThreadSafe> Protocol;
	FName ErrorCodePrefix;
	EUnrealAIErrorCategory ForbiddenErrorCategory = EUnrealAIErrorCategory::NotAuthorized;
	int32 MaximumActiveRequests = 64;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	bool bDrainEventsSynchronouslyForTesting = false;
	TFunction<void()> BeforeFirstDrainForTesting;
	TFunction<void()> AfterFirstQueuedDrainBodyForTesting;
#endif

	bool ValidateShape(FString &OutError) const;
};

/**
 * Lifecycle-managed native SSE provider over the credential-safe HTTPS transport.
 *
 * This class owns no provider wire grammar. It centralizes exact connection and
 * model allowlists, staged callback admission, bounded event retention,
 * continuation commit ordering, cancellation, and physical-settlement tracking.
 */
class UNREALAI_API FUnrealAINativeSseModelProvider final : public IUnrealAIModelProvider
{
  public:
	FUnrealAINativeSseModelProvider(
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport,
		const FUnrealAINativeSseModelProviderConfig &InConfig);
	~FUnrealAINativeSseModelProvider() override;

	FUnrealAINativeSseModelProvider(const FUnrealAINativeSseModelProvider &) = delete;
	FUnrealAINativeSseModelProvider &operator=(const FUnrealAINativeSseModelProvider &) = delete;

	FName GetProviderName() const override;
	FUnrealAIModelProviderDescriptor Describe() const override;
	EUnrealAIModelCapability GetCapabilities(const FString &ModelId) const override;
	FUnrealAIModelProfileProjection GetModelProjection(const FString &ModelId) const override;
	bool StartRequest(const FUnrealAIModelRequest &Request,
					  TSharedRef<const IUnrealAIProviderAccessContext, ESPMode::ThreadSafe> AccessContext,
					  TSharedRef<IUnrealAIModelEventSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIModelRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIModelError &OutError) override;
	void BeginShutdown() override;

  private:
	class FState;
	TSharedRef<FState, ESPMode::ThreadSafe> State;
};
