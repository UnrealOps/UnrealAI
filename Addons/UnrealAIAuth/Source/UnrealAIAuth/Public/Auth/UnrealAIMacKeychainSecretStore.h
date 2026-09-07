// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

/**
 * Apple Keychain generic-password backend (the class name is retained for source compatibility). Secret values never
 * cross this API as strings and record identifiers are never logged or reflected.
 *
 * Other platforms compile a deterministic NotSupported implementation so the
 * provider-free and minimal build matrices remain portable.
 */
class UNREALAIAUTH_API FUnrealAIMacKeychainSecretStore final : public IUnrealAISecretStore
{
  public:
	FUnrealAIMacKeychainSecretStore();
	~FUnrealAIMacKeychainSecretStore() override;
	FUnrealAIMacKeychainSecretStore(const FUnrealAIMacKeychainSecretStore &) = delete;
	FUnrealAIMacKeychainSecretStore &operator=(const FUnrealAIMacKeychainSecretStore &) = delete;

	static FName StoreName();
	static bool IsPlatformSupported();

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

  private:
#if PLATFORM_APPLE && (WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS)
	friend class FUnrealAIMacKeychainSecretStoreFaultMappingTest;

	/** Closed, secret-free statuses for the one-shot deterministic platform-call automation seam. */
	enum class EAutomationPlatformStatus : uint8
	{
		Succeeded,
		InteractionNotAllowed,
		AuthFailed,
		NotAvailable,
		Decode
	};

	/** Replaces the next platform call and runs the callback after its simulated return. */
	void SetAutomationPlatformCallOverride(EAutomationPlatformStatus Status, TFunction<void()> AfterPlatformCall = {});
#endif

	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
