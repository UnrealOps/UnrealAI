// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIConnectionRegistry.h"
#include "CoreMinimal.h"
#include "Models/UnrealAIModelProvider.h"
#include "OpenAI/UnrealAIOpenAIResponsesProtocol.h"
#include "Transport/UnrealAIHttpTransport.h"

/** Exact trusted wire policy for one Responses-compatible provider registration. */
struct UNREALAI_API FUnrealAIOpenAIResponsesProviderConfig final
{
	static constexpr int32 MaxFixedHeaders = 8;
	static constexpr int32 MaxModelProfiles = 32;

	/** Optional monotonic clock for deterministic deadline simulation. */
	TSharedPtr<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FName ProviderName;
	FName AccountAuthProviderName;
	EUnrealAIAuthScheme AuthScheme = EUnrealAIAuthScheme::Invalid;
	EUnrealAIBillingMode BillingMode = EUnrealAIBillingMode::Invalid;
	FUnrealAIEndpointOrigin EndpointOrigin;
	FString Audience;
	FString RelativePath;
	EUnrealAIHttpCredentialPresentation CredentialPresentation = EUnrealAIHttpCredentialPresentation::Invalid;
	/** Optional provider-owned name for the protected secondary credential header. The value remains transport-only. */
	FString ProtectedSecondaryHeaderName;
	TArray<FUnrealAIHttpRequestHeader> FixedHeaders;
	/** Stable public-safe status outcomes used by the bounded auth replay and entitlement layers. */
	FName UnauthorizedErrorCode = TEXT("provider_http_unauthorized");
	FName ForbiddenErrorCode = TEXT("provider_http_forbidden");
	EUnrealAIErrorCategory ForbiddenErrorCategory = EUnrealAIErrorCategory::NotAuthorized;
	/** Provider-owned public fault namespace; the shared wire implementation does not register a provider. */
	FUnrealAIOpenAIResponsesPublicFaultPolicy PublicFaultPolicy;
	/** Defaults copied into exact tool-capable profiles created through MakeToolCapableModelProfile. */
	int32 DefaultMaximumToolsPerRequest = 64;
	int32 DefaultMaximumInputMessages = FUnrealAIModelRequest::MaxInputMessages;
	int32 DefaultMaximumOutputTokens = 4096;
	/** Optional exact model overrides. Capabilities must remain within the adapter's normalized surface. */
	TArray<FUnrealAIModelProfileProjection> ModelProfiles;
	/**
	 * When true, ModelProfiles is an exact allowlist and unconfigured model IDs
	 * fail before credential or transport work. Compatibility callers may opt
	 * into the conservative text-only fallback by setting this false explicitly.
	 */
	bool bRequireConfiguredModelProfile = false;
	/** Uses the same native execution path with the conservative Chat Completions codec. */
	bool bUseChatCompletions = false;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	/** Deterministic adapter-lifetime seam; production drains always use the bounded worker path. */
	bool bDrainEventsSynchronouslyForTesting = false;
	/** Optional one-shot barrier invoked on the first worker drain, before any staged event is processed. */
	TFunction<void()> BeforeFirstDrainForTesting;
	/**
	 * Optional one-shot barrier invoked from the first queued drain closure's
	 * destructor after its body and captured operation owners have returned.
	 */
	TFunction<void()> AfterFirstQueuedDrainBodyForTesting;
	/** Test-only projection of the production active-operation bound. */
	int32 MaximumActiveRequestsForTesting = 256;
#endif

	bool ValidateShape(FString &OutError) const;
	/** Builds one editable exact tool-capable profile within the normalized Responses surface. */
	FUnrealAIModelProfileProjection MakeToolCapableModelProfile(const FString &ModelId) const;
	/** Builds one exact public Responses profile with tools, strict JSON output, and bounded image input. */
	FUnrealAIModelProfileProjection MakePublicMultimodalModelProfile(const FString &ModelId) const;
	static FUnrealAIOpenAIResponsesProviderConfig OpenAIPlatformApiKey();
};

/** First-party OpenAI Responses model provider over an injected credential-safe HTTPS transport. */
class UNREALAI_API FUnrealAIOpenAIResponsesProvider final : public IUnrealAIModelProvider
{
  public:
	FUnrealAIOpenAIResponsesProvider(
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport);
	FUnrealAIOpenAIResponsesProvider(
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<IUnrealAIHttpTransport, ESPMode::ThreadSafe> InTransport,
		const FUnrealAIOpenAIResponsesProviderConfig &InConfig);
	~FUnrealAIOpenAIResponsesProvider() override;

	FUnrealAIOpenAIResponsesProvider(const FUnrealAIOpenAIResponsesProvider &) = delete;
	FUnrealAIOpenAIResponsesProvider &operator=(const FUnrealAIOpenAIResponsesProvider &) = delete;

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
