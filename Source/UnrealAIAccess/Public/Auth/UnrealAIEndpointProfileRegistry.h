// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

/** Immutable authoritative endpoint-policy view consumed by a connection registry and later HTTP transport. */
class UNREALAIACCESS_API FUnrealAIEndpointProfileRegistrySnapshot final
{
  public:
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Find(FName ProfileId) const;
	TArray<TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>> GetProfiles() const;
	uint64 GetGeneration() const
	{
		return Generation;
	}
	int32 Num() const;

  private:
	friend class FUnrealAIEndpointProfileRegistry;
	struct FImpl;
	explicit FUnrealAIEndpointProfileRegistrySnapshot(TSharedRef<const FImpl, ESPMode::ThreadSafe> InImpl,
													  uint64 InGeneration);
	TSharedRef<const FImpl, ESPMode::ThreadSafe> Impl;
	uint64 Generation = 0;
};

/**
 * Typed endpoint admission. There is deliberately no generic "register descriptor" method: custom API endpoints and
 * provider-approved subscription resources enter through different trust paths.
 */
class UNREALAIACCESS_API FUnrealAIEndpointProfileRegistry final
{
  public:
	static constexpr int32 MaxProfiles = 256;

	FUnrealAIEndpointProfileRegistry();
	~FUnrealAIEndpointProfileRegistry();
	FUnrealAIEndpointProfileRegistry(const FUnrealAIEndpointProfileRegistry &) = delete;
	FUnrealAIEndpointProfileRegistry &operator=(const FUnrealAIEndpointProfileRegistry &) = delete;

	bool RegisterLocalInProcessEndpoint(
		FName ProfileId, FName ModelProviderName, uint64 PolicyRevision,
		TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered, FString &OutError);
	bool
	RegisterCustomApiEndpoint(FName ProfileId, FName ModelProviderName, const FUnrealAIEndpointOrigin &Origin,
							  const FString &Audience, uint64 PolicyRevision,
							  TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered,
							  FString &OutError);
	bool RegisterProjectGatewayEndpoint(
		FName ProfileId, FName ModelProviderName, const FUnrealAIEndpointOrigin &Origin, const FString &Audience,
		uint64 PolicyRevision, TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered,
		FString &OutError);
	bool RegisterProviderSubscriptionEndpoint(
		const FUnrealAIProviderEndpointAuthority &Authority, FName ProfileId, FName ModelProviderName,
		const FUnrealAIEndpointOrigin &Origin, const FString &Audience, uint64 PolicyRevision,
		TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered, FString &OutError);

	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Find(FName ProfileId) const;
	TSharedRef<const FUnrealAIEndpointProfileRegistrySnapshot, ESPMode::ThreadSafe> CreateSnapshot() const;
	int32 Num() const;
	uint64 GetGeneration() const;
	static bool IsProfileClassAllowedInBuild(EUnrealAIEndpointProfileClass ProfileClass, bool bShippingBuild);

  private:
	bool RegisterNormalized(FUnrealAIEndpointProfileDescriptor &&Descriptor,
							TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered,
							FString &OutError);
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
