// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIConnectionRegistry.h"
#include "Auth/UnrealAIOAuthTokenEnvelope.h"
#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

namespace UE::UnrealAI::Private
{
class FOAuthCredentialBrokerState;
}

/** Non-secret binding from one exact local OAuth account to one atomic secure-store token envelope. */
struct UNREALAIAUTH_API FUnrealAIOAuthCredentialBinding final
{
	FName ProviderName;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FUnrealAISecretHandle SecretHandle;
	bool bRequiresProtectedSecondary = false;

	bool ValidateShape(FString &OutError) const;
};

/**
 * Provider-specific refresh boundary called only on a broker worker.
 *
 * Implementations mint the refresh request from the envelope, use an exact-origin sensitive HTTP transport, validate
 * the response, and atomically apply the returned token set to InOutEnvelope. They never persist or expose tokens.
 */
class UNREALAIAUTH_API IUnrealAIOAuthCredentialRefreshSource
{
  public:
	virtual ~IUnrealAIOAuthCredentialRefreshSource() = default;
	virtual FName GetProviderName() const = 0;
	virtual bool Refresh(FUnrealAIOAuthTokenEnvelope &InOutEnvelope, double TimeoutSeconds,
						 const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) = 0;
};

/** Bounded non-reflected policy for one provider-specific OAuth broker. */
struct UNREALAIAUTH_API FUnrealAIOAuthCredentialBrokerConfig final
{
	static constexpr int32 HardMaxConcurrentOperations = 64;
	static constexpr double MaxRefreshSkewSeconds = 15.0 * 60.0;

	int32 MaxConcurrentOperations = 8;
	double LeaseLifetimeSeconds = 60.0;
	double RefreshSkewSeconds = 120.0;

	bool ValidateShape(FString &OutError) const;
};

/**
 * OAuth credential broker with atomic Keychain envelopes and per-account single-flight refresh.
 *
 * Every resolve freezes one connection descriptor, loads one versioned envelope, refreshes near expiry at most once
 * per account, commits rotating refresh credentials with secure-store compare-and-swap, and creates one exact-bound
 * short-lived lease. Cancellation, timeout, invalidation, and shutdown publish at most one logical terminal even if a
 * platform store or network call physically returns later.
 */
class UNREALAIAUTH_API FUnrealAIOAuthCredentialBroker final : public IUnrealAIRefreshableCredentialBroker
{
  public:
	FUnrealAIOAuthCredentialBroker(
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		TSharedRef<IUnrealAIOAuthCredentialRefreshSource, ESPMode::ThreadSafe> InRefreshSource,
		const FUnrealAIOAuthCredentialBrokerConfig &InConfig = FUnrealAIOAuthCredentialBrokerConfig{});
	~FUnrealAIOAuthCredentialBroker() override;
	FUnrealAIOAuthCredentialBroker(const FUnrealAIOAuthCredentialBroker &) = delete;
	FUnrealAIOAuthCredentialBroker &operator=(const FUnrealAIOAuthCredentialBroker &) = delete;

	bool RegisterBinding(const FUnrealAIOAuthCredentialBinding &Binding, FString &OutError);

	bool StartResolve(const FUnrealAICredentialRequest &Request,
					  TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError) override;
	void InvalidateAccount(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) override;
	void InvalidateAuthProfile(FName AuthProfileId) override;
	void InvalidateConnection(FName ConnectionAlias) override;
	void BeginShutdown() override;

	/**
	 * Publishes a trusted successful secure-store replacement.
	 *
	 * This is the only ordinary lifecycle transition that clears reauthentication quarantine. NewRevision must be the
	 * nonzero revision returned by the successful atomic store commit. Before clearing quarantine, the broker reloads
	 * that exact revision under the caller's cancellation/deadline context and validates its account-bound refresh and
	 * protected-secondary material.
	 */
	bool NotifyCredentialReplaced(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId, uint64 NewRevision,
								  double TimeoutSeconds, const FUnrealAICancellationToken &Cancellation,
								  FString &OutError);
	/** Revokes existing leases and rejects future resolves until a trusted replacement is committed. */
	void QuarantineAccountForReauthentication(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) override;
	/** Returns a public-safe quarantine projection without exposing provider entitlement details. */
	bool TryGetAccountQuarantine(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId,
								 FUnrealAIProviderAccessError &OutError) const;
	/** Forces the next resolve to refresh even when the loaded access-token expiry is outside the normal skew. */
	void ForceRefreshAccount(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) override;
	bool IsShutdown() const;
	int32 GetActiveOperationCount() const;
	int32 GetPhysicalOperationCount() const;
	int32 GetRefreshFlightCount() const;

  private:
	TSharedRef<UE::UnrealAI::Private::FOAuthCredentialBrokerState, ESPMode::ThreadSafe> State;
};
