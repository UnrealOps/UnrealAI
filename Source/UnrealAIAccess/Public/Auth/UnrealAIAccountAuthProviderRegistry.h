// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

/** Immutable point-in-time view of registered account-auth providers. */
class UNREALAIACCESS_API FUnrealAIAccountAuthProviderRegistrySnapshot final
{
  public:
	TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Find(FName ProviderName) const;
	TArray<FName> GetProviderNames() const;
	uint64 GetGeneration() const
	{
		return Generation;
	}
	int32 Num() const;

  private:
	friend class FUnrealAIAccountAuthProviderRegistry;
	struct FImpl;
	explicit FUnrealAIAccountAuthProviderRegistrySnapshot(TSharedRef<const FImpl, ESPMode::ThreadSafe> InImpl,
														  uint64 InGeneration);
	TSharedRef<const FImpl, ESPMode::ThreadSafe> Impl;
	uint64 Generation = 0;
};

/**
 * Bounded trusted registry and sole minting path for provider endpoint authority. A provider implementation cannot
 * authorize a subscription resource until its unique provider name has been admitted here.
 */
class UNREALAIACCESS_API FUnrealAIAccountAuthProviderRegistry final
{
  public:
	static FUnrealAIAccountAuthProviderRegistry &GetGlobal();
	static constexpr int32 MaxProviders = 32;

	FUnrealAIAccountAuthProviderRegistry();
	~FUnrealAIAccountAuthProviderRegistry();
	FUnrealAIAccountAuthProviderRegistry(const FUnrealAIAccountAuthProviderRegistry &) = delete;
	FUnrealAIAccountAuthProviderRegistry &operator=(const FUnrealAIAccountAuthProviderRegistry &) = delete;

	bool Register(TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider,
				  FUnrealAIProviderEndpointAuthority &OutAuthority, FString &OutError);
	/** Removes only the exact registered instance for composition rollback or owner shutdown. */
	bool Unregister(TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider, FString &OutError);
	TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Find(FName ProviderName) const;
	TSharedRef<const FUnrealAIAccountAuthProviderRegistrySnapshot, ESPMode::ThreadSafe> CreateSnapshot() const;
	int32 Num() const;
	uint64 GetGeneration() const;

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
