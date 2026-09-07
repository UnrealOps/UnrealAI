// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIAccountAuthProviderRegistry.h"
#include "Auth/UnrealAIConnectionRegistry.h"
#include "CoreMinimal.h"

/** One bounded, non-secret account choice published by trusted provider composition code. */
struct UNREALAIACCESS_API FUnrealAIAccountCatalogEntry final
{
	static constexpr int32 MaxDisplayLabelUtf8Bytes = 256;

	/** Exact opaque choice key. Partial, provider-only, and display-label lookup are intentionally unsupported. */
	FName SelectionAlias;
	/** Product-authored, non-secret label. Provider subjects, email addresses, and token claims are forbidden. */
	FString DisplayLabel;
	FName ProviderName;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;
	FName ConnectionAlias;

	bool ValidateShape(FString &OutError) const;
};

/** Dynamic safe view used by account UI and exact selection. It contains no credential or provider identity claim. */
struct UNREALAIACCESS_API FUnrealAIAccountCatalogView final
{
	FUnrealAIAccountCatalogEntry Entry;
	FUnrealAIAccountStatus Status;
	FUnrealAIProviderAccessDescriptor Access;
	FUnrealAICredentialDestination Destination;
	/** False for a stale/deleted registration or any provider, account, connection, payer, or destination drift. */
	bool bBindingCurrent = false;
};

/** Immutable point-in-time membership view with live, fail-closed status checks. */
class UNREALAIACCESS_API FUnrealAIAccountCatalogSnapshot final
{
  public:
	TArray<FUnrealAIAccountCatalogView> List() const;
	/** Resolves only one exact alias whose complete binding is current and whose account is ready. */
	bool ResolveReadyExact(FName SelectionAlias, FUnrealAIAccountCatalogView &OutSelection,
						   FUnrealAIProviderAccessError &OutError) const;
	uint64 GetGeneration() const
	{
		return Generation;
	}
	int32 Num() const;

  private:
	friend class FUnrealAIAccountCatalogRegistry;
	struct FImpl;
	explicit FUnrealAIAccountCatalogSnapshot(TSharedRef<const FImpl, ESPMode::ThreadSafe> InImpl, uint64 InGeneration);
	TSharedRef<const FImpl, ESPMode::ThreadSafe> Impl;
	uint64 Generation = 0;
};

/**
 * Bounded catalog of exact local account choices. The catalog deliberately exposes no login/logout operation;
 * mutations still require FUnrealAITrustedLocalAuthGesture through a trusted local UI authority.
 */
class UNREALAIACCESS_API FUnrealAIAccountCatalogRegistry final
{
  public:
	static FUnrealAIAccountCatalogRegistry &GetGlobal();
	static constexpr int32 MaxEntries = 128;

	explicit FUnrealAIAccountCatalogRegistry(FUnrealAIAccountAuthProviderRegistry &InAuthProviders);
	~FUnrealAIAccountCatalogRegistry();
	FUnrealAIAccountCatalogRegistry(const FUnrealAIAccountCatalogRegistry &) = delete;
	FUnrealAIAccountCatalogRegistry &operator=(const FUnrealAIAccountCatalogRegistry &) = delete;

	/** Registers one already-authorized provider/connection binding. Duplicate aliases are always rejected. */
	bool Register(const FUnrealAIAccountCatalogEntry &Entry,
				  TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider,
				  TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Connection, FString &OutError);
	/** Withdraws only the exact provider instance and invalidates already-created snapshots. */
	bool Unregister(FName SelectionAlias, TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider,
					FString &OutError);
	TSharedRef<const FUnrealAIAccountCatalogSnapshot, ESPMode::ThreadSafe> CreateSnapshot() const;
	int32 Num() const;
	uint64 GetGeneration() const;

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
