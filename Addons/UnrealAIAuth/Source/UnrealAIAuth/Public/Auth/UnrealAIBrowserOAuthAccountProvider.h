// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthAuthorizationCoordinator.h"
#include "Auth/UnrealAIOAuthCredentialBroker.h"
#include "Auth/UnrealAIOAuthDurableAccountTransaction.h"
#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

namespace UE::UnrealAI::Private
{
class FBrowserOAuthAccountProviderState;
}

/** Fixed, non-secret public-client policy used to mint each browser-PKCE authorization request. */
struct UNREALAIAUTH_API FUnrealAIBrowserOAuthAuthorizationPolicy final
{
	FUnrealAIOAuthTrustedAuthorizationServer Server;
	FString ClientId;
	FString Audience;
	FString ExactRedirectUri;
	TArray<FString> RequestedScopes;
	FString ExpectedSubjectFingerprint;
	double ClockSkewSeconds = 60.0;
	double MaxIdentityTokenAgeSeconds = 15.0 * 60.0;
	double MaxJwksCacheAgeSeconds = 60.0 * 60.0;

	bool ValidateShape(FString &OutError) const;
	FUnrealAIOAuthBrowserAuthorizationRequest MakeRequest(const FUnrealAIRequestId &RequestId,
														  double TimeoutSeconds) const;
};

/** Fixed non-secret policy for one durable browser-OAuth account. */
struct UNREALAIAUTH_API FUnrealAIBrowserOAuthAccountProviderConfig final
{
	FName ProviderName;
	FName ModelProviderName;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FUnrealAISecretHandle SecretHandle;
	FUnrealAIBrowserOAuthAuthorizationPolicy Authorization;
	EUnrealAIProviderAccessSupportClassification SupportClassification =
		EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
	double CleanupTimeoutSeconds = FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds;
	bool bRequireRefreshToken = true;
	bool bRequireAccountRoutingValue = false;

	bool ValidateShape(FString &OutError) const;
};

/**
 * Provider-neutral browser-PKCE account lifecycle over one durable protected-store record.
 *
 * Sign-in runs the authorize-and-commit transaction on a retained worker, publishes the exact committed revision to
 * the credential broker, and cannot become Ready after cancellation or a newer sign-out epoch. Sign-out fences and
 * cancels sign-in/recovery first, invalidates leases, waits for physical mutation settlement, revokes through the
 * retained coordinator under a fresh bounded lineage, and only then deletes the exact protected-store revision.
 */
class UNREALAIAUTH_API FUnrealAIBrowserOAuthAccountProvider final : public IUnrealAIAccountAuthProvider
{
  public:
	FUnrealAIBrowserOAuthAccountProvider(
		const FUnrealAIBrowserOAuthAccountProviderConfig &InConfig,
		TSharedRef<FUnrealAIOAuthDurableAccountTransaction, ESPMode::ThreadSafe> InTransaction,
		TSharedRef<FUnrealAIOAuthAuthorizationCoordinator, ESPMode::ThreadSafe> InAuthorizationCoordinator,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock);
	~FUnrealAIBrowserOAuthAccountProvider() override;
	FUnrealAIBrowserOAuthAccountProvider(const FUnrealAIBrowserOAuthAccountProvider &) = delete;
	FUnrealAIBrowserOAuthAccountProvider &operator=(const FUnrealAIBrowserOAuthAccountProvider &) = delete;

	/** Connects the composition-owned broker before the provider is exposed for normal use. */
	bool SetCredentialBroker(TSharedRef<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> InBroker,
							 FString &OutError);
	/** Starts one bounded protected-store recovery without opening a browser. */
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
	TSharedRef<UE::UnrealAI::Private::FBrowserOAuthAccountProviderState, ESPMode::ThreadSafe> State;
};
