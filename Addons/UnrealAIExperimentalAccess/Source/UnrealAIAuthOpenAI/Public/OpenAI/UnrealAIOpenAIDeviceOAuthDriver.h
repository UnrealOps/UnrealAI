// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIDeviceOAuthAccountProvider.h"
#include "CoreMinimal.h"
#include "Transport/UnrealAIOAuthHttpTransport.h"

class FEvent;

/**
 * Bounded wait seam for the reviewed OpenAI device flow. Production waits in cancellation-aware increments;
 * deterministic tests can advance the injected clock and release deferred completions without sleeping.
 */
class UNREALAIAUTHOPENAI_API IUnrealAIOpenAIOAuthWaiter
{
  public:
	virtual ~IUnrealAIOpenAIOAuthWaiter() = default;
	virtual bool WaitForExchange(FEvent &CompletionEvent, double MaxWaitSeconds) = 0;
	virtual bool WaitForPoll(double Seconds, double OverallDeadlineSeconds, const IUnrealAIClock &Clock,
							 const FUnrealAICancellationToken &Cancellation) = 0;
};

/** Exact reviewed OpenAI Codex device-authorization and refresh compatibility driver. */
class UNREALAIAUTHOPENAI_API FUnrealAIOpenAIDeviceOAuthDriver final : public IUnrealAIDeviceOAuthAuthorizationDriver
{
  public:
	FUnrealAIOpenAIDeviceOAuthDriver(TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> InTransport,
									 TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
									 TSharedPtr<IUnrealAIOpenAIOAuthWaiter, ESPMode::ThreadSafe> InWaiter = nullptr);
	~FUnrealAIOpenAIDeviceOAuthDriver() override;
	FUnrealAIOpenAIDeviceOAuthDriver(const FUnrealAIOpenAIDeviceOAuthDriver &) = delete;
	FUnrealAIOpenAIDeviceOAuthDriver &operator=(const FUnrealAIOpenAIDeviceOAuthDriver &) = delete;

	FName GetProviderName() const override;
	bool Authorize(double TimeoutSeconds, const FUnrealAICancellationToken &Cancellation,
				   IUnrealAIDeviceOAuthInteractionPublisher &InteractionPublisher, FUnrealAIOAuthTokenSet &OutTokens,
				   FUnrealAIProviderAccessError &OutError) override;
	bool Refresh(FUnrealAIOAuthTokenEnvelope &InOutEnvelope, double TimeoutSeconds,
				 const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) override;

	static int32 GetCompatibilityRevision();

  private:
	class FState;
	TSharedRef<FState, ESPMode::ThreadSafe> State;
};
