// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

/**
 * Process-memory-only secure-store implementation for deterministic tests and explicitly ephemeral deployments.
 * It never persists, serializes, logs, or reflects credentials. Every replacement and destruction wipes owned bytes.
 */
class UNREALAIACCESS_API FUnrealAIMemorySecretStore final : public IUnrealAISecretStore
{
  public:
	static constexpr int32 DefaultMaxRecords = 256;
	static constexpr int32 HardMaxRecords = 4096;

	explicit FUnrealAIMemorySecretStore(FName InStoreName, int32 InMaxRecords = DefaultMaxRecords);
	~FUnrealAIMemorySecretStore() override;
	FUnrealAIMemorySecretStore(const FUnrealAIMemorySecretStore &) = delete;
	FUnrealAIMemorySecretStore &operator=(const FUnrealAIMemorySecretStore &) = delete;

	FName GetStoreName() const override;
	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override;
	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &Context,
									const FUnrealAISecretHandle &Handle, FUnrealAISecretValue &OutValue,
									uint64 &OutRevision, FUnrealAIProviderAccessError &OutError) override;
	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &Context,
									 const FUnrealAISecretHandle &Handle, const FUnrealAISecretValue &Value,
									 uint64 ExpectedRevision, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override;
	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &Context,
									  const FUnrealAISecretHandle &Handle, uint64 ExpectedRevision,
									  FUnrealAIProviderAccessError &OutError) override;

	void Clear();
	int32 Num() const;

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
