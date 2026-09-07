// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIApiKeyCredentialBroker.h"
#include "Auth/UnrealAIApiKeyProvisioning.h"
#include "CoreMinimal.h"
#include "Runtime/UnrealAIClock.h"

/** Exact non-secret binding for one setup-only provider API-key record. */
struct UNREALAIAUTH_API FUnrealAIApiKeyProvisionerConfig final
{
	static constexpr int32 MaximumConcurrentOperationsLimit = 64;

	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FUnrealAISecretHandle SecretHandle;
	int32 MaximumConcurrentOperations = 4;

	bool ValidateShape(FString &OutError) const;
};

/**
 * Bounded, terminal-once API-key store/delete coordinator.
 *
 * Secret bytes move directly into an off-thread secure-store operation and
 * are wiped on every visible terminal path. Successful mutations invalidate
 * the exact broker account so no prior lease can silently survive a key
 * rotation or deletion.
 */
class UNREALAIAUTH_API FUnrealAIApiKeyProvisioner final
	: public IUnrealAIApiKeyProvisioner,
	  public TSharedFromThis<FUnrealAIApiKeyProvisioner, ESPMode::ThreadSafe>
{
  public:
	static bool TryCreate(const FUnrealAIApiKeyProvisionerConfig &Config,
						  TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore,
						  TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
						  TSharedRef<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> Broker,
						  TSharedPtr<FUnrealAIApiKeyProvisioner, ESPMode::ThreadSafe> &OutProvisioner,
						  FString &OutError);

	~FUnrealAIApiKeyProvisioner() override;
	FUnrealAIApiKeyProvisioner(const FUnrealAIApiKeyProvisioner &) = delete;
	FUnrealAIApiKeyProvisioner &operator=(const FUnrealAIApiKeyProvisioner &) = delete;

	bool StartStore(const FUnrealAIRequestId &RequestId, FUnrealAISecretValue &&ApiKey, float TimeoutSeconds,
					TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink,
					const FUnrealAICancellationToken &Cancellation,
					TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> &OutHandle,
					FUnrealAIProviderAccessError &OutError) override;
	bool StartDelete(const FUnrealAIRequestId &RequestId, float TimeoutSeconds,
					 TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink,
					 const FUnrealAICancellationToken &Cancellation,
					 TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> &OutHandle,
					 FUnrealAIProviderAccessError &OutError) override;

	void BeginShutdown() override;
	bool IsShutdown() const override;
	int32 GetActiveOperationCount() const override;

  private:
	class FState;
	explicit FUnrealAIApiKeyProvisioner(TSharedRef<FState, ESPMode::ThreadSafe> InState);
	bool Start(const FUnrealAIRequestId &RequestId, bool bDelete, FUnrealAISecretValue &&Secret, float TimeoutSeconds,
			   TSharedRef<IUnrealAIApiKeyProvisionSink, ESPMode::ThreadSafe> Sink,
			   const FUnrealAICancellationToken &Cancellation,
			   TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> &OutHandle,
			   FUnrealAIProviderAccessError &OutError);

	TSharedRef<FState, ESPMode::ThreadSafe> State;
};
