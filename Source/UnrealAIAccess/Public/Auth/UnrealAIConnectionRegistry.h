// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "CoreMinimal.h"

/** Immutable point-in-time view of trusted, non-secret provider connections. */
class UNREALAIACCESS_API FUnrealAIConnectionRegistrySnapshot final
{
  public:
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Find(FName ConnectionAlias) const;
	TArray<TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>> GetConnections() const;
	uint64 GetGeneration() const
	{
		return Generation;
	}
	int32 Num() const;

  private:
	friend class FUnrealAIConnectionRegistry;
	struct FImpl;
	explicit FUnrealAIConnectionRegistrySnapshot(TSharedRef<const FImpl, ESPMode::ThreadSafe> InImpl,
												 uint64 InGeneration);
	TSharedRef<const FImpl, ESPMode::ThreadSafe> Impl;
	uint64 Generation = 0;
};

/** Thread-safe bounded registry. Published connection descriptors never mutate. */
class UNREALAIACCESS_API FUnrealAIConnectionRegistry final
{
  public:
	static constexpr int32 MaxConnections = 1024;

	explicit FUnrealAIConnectionRegistry(
		TSharedRef<const FUnrealAIEndpointProfileRegistrySnapshot, ESPMode::ThreadSafe> InEndpointProfiles);
	~FUnrealAIConnectionRegistry();
	FUnrealAIConnectionRegistry(const FUnrealAIConnectionRegistry &) = delete;
	FUnrealAIConnectionRegistry &operator=(const FUnrealAIConnectionRegistry &) = delete;

	bool Register(const FUnrealAIConnectionDescriptor &Descriptor,
				  TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> &OutRegistered,
				  FString &OutError);
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Find(FName ConnectionAlias) const;
	TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> CreateSnapshot() const;
	int32 Num() const;
	uint64 GetGeneration() const;

	/** Returns false unless an automatic fallback preserves the frozen destination and payer binding. */
	static bool CanFallbackWithoutPayerChange(const FUnrealAIConnectionDescriptor &Primary,
											  const FUnrealAIConnectionDescriptor &Candidate, FString &OutError);

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
