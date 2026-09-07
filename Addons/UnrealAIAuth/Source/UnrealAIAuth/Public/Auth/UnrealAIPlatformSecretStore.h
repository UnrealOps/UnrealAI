// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

/** Closed deployment purpose for selecting a reviewed platform/project credential store. */
enum class EUnrealAIPlatformSecretStorePurpose : uint8
{
	Invalid,
	InteractiveCurrentUser,
	AuthoritativeService
};

/** Truthful requirements used to select a store without hard-coding one platform adapter. */
struct UNREALAIAUTH_API FUnrealAIPlatformSecretStoreSelection final
{
	EUnrealAIPlatformSecretStorePurpose Purpose = EUnrealAIPlatformSecretStorePurpose::Invalid;
	bool bRequirePersistent = true;
	bool bRequireAtomicCompareAndSwap = false;
	bool bRequireAvailableInShipping = false;

	static FUnrealAIPlatformSecretStoreSelection CurrentUserApiKey(bool bForShipping = false);
	static FUnrealAIPlatformSecretStoreSelection CurrentUserOAuth(bool bForShipping = false);
	static FUnrealAIPlatformSecretStoreSelection AuthoritativeService(bool bForShipping = false);
	bool ValidateShape(FString &OutError) const;
};

/**
 * Public factory for the reviewed credential store on the current deployment.
 *
 * A platform/project module may register one exact adapter for each deployment purpose during startup. Interactive
 * adapters must advertise a persistent current-user platform credential store; authoritative adapters must advertise
 * a persistent service-scoped external secret service. Selection revalidates the exact instance and capabilities on
 * every use. Apple Keychain is the built-in current-user fallback on supported Apple builds. There is no plaintext,
 * process-memory, local-machine-for-user, or purpose-changing fallback.
 */
class UNREALAIAUTH_API FUnrealAIPlatformSecretStore final
{
  public:
	/**
	 * Registers one process-wide reviewed adapter for an exact purpose. Duplicate purposes, unsafe scope/protection,
	 * capability drift, and settings-capability registration after publication all fail closed.
	 */
	static bool RegisterAdapter(EUnrealAIPlatformSecretStorePurpose Purpose,
								TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store, FString &OutError);
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	/** Automation-only registration that exercises selection without permanently mutating settings capability state. */
	static bool RegisterAdapterForAutomation(EUnrealAIPlatformSecretStorePurpose Purpose,
											 TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store,
											 FString &OutError);
#endif
	/** Withdraws only the exact registered instance. Already-created users retain their strong reference safely. */
	static bool UnregisterAdapter(EUnrealAIPlatformSecretStorePurpose Purpose,
								  TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store, FString &OutError);

	/** NAME_None when this build has no supported built-in platform store. */
	static FName GetCompiledStoreName();
	/** Empty/unavailable capabilities when the selection cannot be served by a built-in store. */
	static FUnrealAISecretStoreCapabilities
	DescribeCompiledCapabilities(const FUnrealAIPlatformSecretStoreSelection &Selection);
	/** NAME_None when no registered or built-in store currently satisfies the exact selection. */
	static FName GetSelectedStoreName(const FUnrealAIPlatformSecretStoreSelection &Selection);
	/** Empty/unavailable capabilities when no registered or built-in store satisfies the exact selection. */
	static FUnrealAISecretStoreCapabilities
	DescribeSelectedCapabilities(const FUnrealAIPlatformSecretStoreSelection &Selection);
	/** Creates the selected registered/built-in store or fails closed with a typed, redacted error. */
	static bool TryCreate(const FUnrealAIPlatformSecretStoreSelection &Selection,
						  TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> &OutStore,
						  FUnrealAISecretStoreCapabilities &OutCapabilities, FUnrealAIProviderAccessError &OutError);

  private:
	static bool RegisterAdapterInternal(EUnrealAIPlatformSecretStorePurpose Purpose,
										TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store,
										bool bRegisterSettingsCapabilities, FString &OutError);
};
