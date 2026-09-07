// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIApiKeyProvisioning.h"
#include "Auth/UnrealAIConnectionRegistry.h"
#include "Models/UnrealAIModelProvider.h"

/** An immutable configured provider. Optional capabilities are absent when their implementation is not installed. */
struct UNREALAI_API FUnrealAIProviderRegistration final
{
	FName ProviderName;
	FName DefaultConnectionAlias;
	TSharedPtr<IUnrealAIModelProvider, ESPMode::ThreadSafe> Provider;
	TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections;
	TSharedPtr<IUnrealAICredentialBroker, ESPMode::ThreadSafe> CredentialBroker;
	TSharedPtr<IUnrealAIApiKeyProvisioner, ESPMode::ThreadSafe> ApiKeyProvisioner;
	FUnrealAIProviderAccessDescriptor Access;

	bool Validate(FString &OutError) const;
};

/** Bounded process catalog of configured SDK providers. Agent/world registration remains with the consumer. */
class UNREALAI_API FUnrealAIProviderCatalog final
{
  public:
	static constexpr int32 MaximumProviders = 32;
	static FUnrealAIProviderCatalog &Get();
	bool Register(TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe> Registration, FString &OutError);
	/** Only the exact registered owner can withdraw an entry. The owner closes its provider and broker first. */
	bool Unregister(TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe> Registration);
	TSharedPtr<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe> Find(FName ProviderName) const;
	TArray<TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe>> Snapshot() const;

  private:
	mutable FCriticalSection Mutex;
	struct FEntry
	{
		TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe> Owner;
		TSharedRef<const FUnrealAIProviderRegistration, ESPMode::ThreadSafe> Value;
	};
	TMap<FName, FEntry> Entries;
};
