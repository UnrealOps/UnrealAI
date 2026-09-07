// Copyright EngineWorks. All Rights Reserved.
#pragma once
#include "Auth/UnrealAIConnectionRegistry.h"
#include "Auth/UnrealAIProviderAccess.h"
/**
 * Non-secret mapping from one exact frozen connection to an opaque secure-store record.
 *
 * ConnectionAlias is mandatory: a key approved for one endpoint policy cannot
 * become eligible for another connection merely because both select the same
 * local auth-profile/account pair.
 */
struct UNREALAIACCESS_API FUnrealAIApiKeyCredentialBinding final
{
	FName ConnectionAlias;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FUnrealAISecretHandle SecretHandle;

	bool ValidateShape(FString &OutError) const;
};

/** Non-reflected resource bounds for the API-key broker. */
struct UNREALAIACCESS_API FUnrealAIApiKeyCredentialBrokerConfig final
{
	static constexpr int32 HardMaxConcurrentStoreOperations = 64;
	static constexpr int32 HardMaxActiveLeases = 4096;

	int32 MaxConcurrentStoreOperations = 8;
	int32 MaxActiveLeases = 256;
	double LeaseLifetimeSeconds = 60.0;

	bool ValidateShape(FString &OutError) const;
};

/** Optional auth capability. No platform storage implementation is linked by callers of this interface. */
class UNREALAIACCESS_API IUnrealAICredentialBrokerFactory
{
  public:
	virtual ~IUnrealAICredentialBrokerFactory() = default;
	virtual bool
	CreateApiKeyBroker(TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections,
					   TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store,
					   TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
					   const FUnrealAIApiKeyCredentialBinding &Binding,
					   const FUnrealAIApiKeyCredentialBrokerConfig &Config,
					   TSharedPtr<IUnrealAICredentialBroker, ESPMode::ThreadSafe> &OutBroker, FString &OutError) = 0;
	static TSharedPtr<IUnrealAICredentialBrokerFactory, ESPMode::ThreadSafe> Get();
	/** One process-lifetime implementation. Registration never replaces a live factory. */
	static bool Register(TSharedRef<IUnrealAICredentialBrokerFactory, ESPMode::ThreadSafe> Factory);
};
