// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIDeviceOAuthAccountProvider.h"
#include "CoreMinimal.h"
#include "Transport/UnrealAIOAuthHttpTransport.h"

/**
 * Bounded polling wait seam. Production sleeps in short cancellation-aware increments; deterministic tests advance
 * an injected fake clock without sleeping.
 */
class UNREALAIAUTHXAI_API IUnrealAIXAIOAuthPollWaiter
{
  public:
	virtual ~IUnrealAIXAIOAuthPollWaiter() = default;
	virtual bool Wait(double Seconds, double OverallDeadlineSeconds, const IUnrealAIClock &Clock,
					  const FUnrealAICancellationToken &Cancellation) = 0;
};

/** Exact reviewed xAI Grok RFC 8628 device-authorization and refresh compatibility driver. */
class UNREALAIAUTHXAI_API FUnrealAIXAIDeviceOAuthDriver final : public IUnrealAIDeviceOAuthAuthorizationDriver
{
  public:
	FUnrealAIXAIDeviceOAuthDriver(TSharedRef<IUnrealAIOAuthHttpTransport, ESPMode::ThreadSafe> InTransport,
								  TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
								  TSharedPtr<IUnrealAIXAIOAuthPollWaiter, ESPMode::ThreadSafe> InPollWaiter = nullptr);
	~FUnrealAIXAIDeviceOAuthDriver() override;
	FUnrealAIXAIDeviceOAuthDriver(const FUnrealAIXAIDeviceOAuthDriver &) = delete;
	FUnrealAIXAIDeviceOAuthDriver &operator=(const FUnrealAIXAIDeviceOAuthDriver &) = delete;

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
