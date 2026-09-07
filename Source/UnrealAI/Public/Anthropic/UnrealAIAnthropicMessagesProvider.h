// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIConnectionRegistry.h"
#include "CoreMinimal.h"
#include "Models/UnrealAIModelProvider.h"
#include "Transport/UnrealAIHttpTransport.h"

class FUnrealAINativeSseModelProvider;

/** Exact first-party Anthropic Messages route and model allowlist. */
struct UNREALAI_API FUnrealAIAnthropicMessagesProviderConfig final
{
	static constexpr int32 MaxModelProfiles = 16;

	FUnrealAIConnectionDescriptor Connection;
	TArray<FUnrealAIModelProfileProjection> ModelProfiles;
	/** Required only when an explicitly configured model profile advertises ImageInput. */
	int32 MaximumActiveRequests = 64;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	bool bDrainEventsSynchronouslyForTesting = false;
#endif

	bool ValidateShape(FString &OutError) const;
	static FUnrealAIModelProfileProjection MakeMessagesModelProfile(const FString &ModelId);
};

/** Native Anthropic Messages implementation of the provider-neutral model SPI. */
class UNREALAI_API FUnrealAIAnthropicMessagesProvider final : public IUnrealAIModelProvider
{
  public:
	FUnrealAIAnthropicMessagesProvider(
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport,
		const FUnrealAIAnthropicMessagesProviderConfig &InConfig);
	~FUnrealAIAnthropicMessagesProvider() override;

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
	TUniquePtr<FUnrealAINativeSseModelProvider> Implementation;
};
