// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIConnectionRegistry.h"
#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"
#include "Auth/UnrealAICredentialBrokerFactory.h"

namespace UE::UnrealAI::Private
{
class FApiKeyCredentialBrokerState;
}

/**
 * Bounded API-key credential broker.
 *
 * Each accepted request resolves its alias once against an immutable connection snapshot, then carries that exact
 * descriptor through secure-store load and lease creation. Blocking platform-store calls run on dedicated worker
 * threads. Logical cancellation, timeout, invalidation, and shutdown publish at most one terminal without joining a
 * hung physical call; the worker retains the backend and wipes/discards any late secret when it eventually returns.
 * OAuth and gateway credentials remain separate broker/provider seams and are rejected by this implementation.
 */
class UNREALAIAUTH_API FUnrealAIApiKeyCredentialBroker final : public IUnrealAICredentialBroker
{
  public:
	FUnrealAIApiKeyCredentialBroker(
		TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> InConnections,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		const FUnrealAIApiKeyCredentialBrokerConfig &InConfig = FUnrealAIApiKeyCredentialBrokerConfig{});
	~FUnrealAIApiKeyCredentialBroker() override;
	FUnrealAIApiKeyCredentialBroker(const FUnrealAIApiKeyCredentialBroker &) = delete;
	FUnrealAIApiKeyCredentialBroker &operator=(const FUnrealAIApiKeyCredentialBroker &) = delete;

	/** Registers setup-only non-secret metadata. Duplicate exact connection aliases fail closed. */
	bool RegisterBinding(const FUnrealAIApiKeyCredentialBinding &Binding, FString &OutError);

	bool StartResolve(const FUnrealAICredentialRequest &Request,
					  TSharedRef<IUnrealAICredentialResultSink, ESPMode::ThreadSafe> Sink,
					  const FUnrealAICancellationToken &Cancellation,
					  TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError) override;

	/** Invalidates only the named profile/account pair, including pending store work and published leases. */
	void InvalidateAccount(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId) override;
	/** Sign-out/quarantine path: revokes exact account work and rejects future resolves for this broker lifetime. */
	void QuarantineAccount(FName AuthProfileId, const FUnrealAIAccessAccountId &AccountId);
	void InvalidateAuthProfile(FName AuthProfileId) override;
	void InvalidateConnection(FName ConnectionAlias) override;
	void BeginShutdown() override;

	bool IsShutdown() const;
	int32 GetPhysicalStoreOperationCount() const;
	/** Public-safe diagnostic count of broker-tracked operations, excluding caller-retained completed handles. */
	int32 GetTrackedOperationCount() const;
	int32 GetActiveLeaseCount() const;
	/** Public-safe diagnostic count; it never exposes sizes, handles, or secret material. */
	int32 GetDiscardedLateSecretCount() const;

  private:
	TSharedRef<UE::UnrealAI::Private::FApiKeyCredentialBrokerState, ESPMode::ThreadSafe> State;
};
