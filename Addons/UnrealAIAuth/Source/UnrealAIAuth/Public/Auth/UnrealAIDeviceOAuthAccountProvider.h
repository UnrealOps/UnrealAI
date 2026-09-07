// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthCredentialBroker.h"
#include "Auth/UnrealAIOAuthTokenEnvelope.h"
#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

namespace UE::UnrealAI::Private
{
class FDeviceOAuthAccountProviderState;
}

/** Trusted interaction publisher available only while one explicit Editor sign-in operation is live. */
class UNREALAIAUTH_API IUnrealAIDeviceOAuthInteractionPublisher
{
  public:
	virtual ~IUnrealAIDeviceOAuthInteractionPublisher() = default;
	/** Takes ownership of a device instruction and rejects late or duplicate publication. */
	virtual bool PublishInteraction(FUnrealAIAuthInteraction &&Interaction) = 0;
};

/**
 * Provider-specific synchronous device authorization driver.
 *
 * The generic account provider schedules this method off the game thread. Implementations use only their compiled
 * authorization policy and injected sensitive HTTP transport, publish at most one device instruction, and return
 * move-only validated token material without persisting it.
 */
class UNREALAIAUTH_API IUnrealAIDeviceOAuthAuthorizationDriver : public IUnrealAIOAuthCredentialRefreshSource
{
  public:
	virtual ~IUnrealAIDeviceOAuthAuthorizationDriver() override = default;
	virtual bool Authorize(double TimeoutSeconds, const FUnrealAICancellationToken &Cancellation,
						   IUnrealAIDeviceOAuthInteractionPublisher &InteractionPublisher,
						   FUnrealAIOAuthTokenSet &OutTokens, FUnrealAIProviderAccessError &OutError) = 0;
};

/** Fixed non-secret policy for one experimental device-OAuth account provider. */
struct UNREALAIAUTH_API FUnrealAIDeviceOAuthAccountProviderConfig final
{
	FName ProviderName;
	FName ModelProviderName;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FUnrealAISecretHandle SecretHandle;
	bool bRequiresProtectedSecondary = false;

	bool ValidateShape(FString &OutError) const;
};

/**
 * Generic device-OAuth account lifecycle with Keychain commit, stale-operation fencing, and terminal-once delivery.
 */
class UNREALAIAUTH_API FUnrealAIDeviceOAuthAccountProvider final : public IUnrealAIAccountAuthProvider
{
  public:
	FUnrealAIDeviceOAuthAccountProvider(
		const FUnrealAIDeviceOAuthAccountProviderConfig &InConfig,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		TSharedRef<IUnrealAIDeviceOAuthAuthorizationDriver, ESPMode::ThreadSafe> InDriver);
	~FUnrealAIDeviceOAuthAccountProvider() override;
	FUnrealAIDeviceOAuthAccountProvider(const FUnrealAIDeviceOAuthAccountProvider &) = delete;
	FUnrealAIDeviceOAuthAccountProvider &operator=(const FUnrealAIDeviceOAuthAccountProvider &) = delete;

	/** Connects the already-constructed broker before registration; the provider never owns it strongly. */
	bool SetCredentialBroker(TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InBroker,
							 FString &OutError);
	/** Starts a bounded off-thread Keychain recovery check for an existing local session. */
	void StartStoredSessionRecovery();
	void BeginShutdown();

	FName GetProviderName() const override;
	FUnrealAIAccountAuthCapabilities DescribeCapabilities() const override;
	FUnrealAIProviderAccessDescriptor DescribeAccess() const override;
	FUnrealAIAccountStatus GetStatus(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) const override;
	bool StartSignIn(const FUnrealAITrustedLocalAuthGesture &Gesture, const FUnrealAIInteractiveAuthRequest &Request,
					 TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
					 const FUnrealAICancellationToken &Cancellation,
					 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					 FUnrealAIProviderAccessError &OutError) override;
	bool StartSignOut(const FUnrealAITrustedLocalAuthGesture &Gesture, const FUnrealAIAccountAuthRequest &Request,
					  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError) override;

	int32 GetActiveOperationCount() const;
	int32 GetPhysicalOperationCount() const;
	bool IsShutdown() const;

  private:
	TSharedRef<UE::UnrealAI::Private::FDeviceOAuthAccountProviderState, ESPMode::ThreadSafe> State;
};
