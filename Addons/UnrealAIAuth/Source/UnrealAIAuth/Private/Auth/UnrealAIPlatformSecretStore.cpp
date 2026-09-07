// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIPlatformSecretStore.h"

#include "Auth/UnrealAIMacKeychainSecretStore.h"
#include "Misc/ScopeLock.h"
#include "Auth/UnrealAICredentialStoreRegistry.h"

namespace
{
struct FRegisteredPlatformSecretStore final
{
	FRegisteredPlatformSecretStore(TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InStore,
								   const FName InStoreName, const FUnrealAISecretStoreCapabilities &InCapabilities)
		: Store(MoveTemp(InStore)), StoreName(InStoreName), Capabilities(InCapabilities)
	{
	}

	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store;
	FName StoreName;
	FUnrealAISecretStoreCapabilities Capabilities;
};

struct FPlatformSecretStoreAdapterRegistry final
{
	FCriticalSection Mutex;
	TMap<EUnrealAIPlatformSecretStorePurpose, FRegisteredPlatformSecretStore> Adapters;
};

FPlatformSecretStoreAdapterRegistry &GetAdapterRegistry()
{
	static FPlatformSecretStoreAdapterRegistry Registry;
	return Registry;
}

FUnrealAIProviderAccessError MakeSelectionError(const EUnrealAIErrorCategory Category,
												const EUnrealAIProviderAccessErrorCode Code)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	return Error;
}

bool CapabilitiesEqual(const FUnrealAISecretStoreCapabilities &A, const FUnrealAISecretStoreCapabilities &B)
{
	return A.PersistenceClass == B.PersistenceClass && A.ProtectionClass == B.ProtectionClass &&
		   A.ScopeClass == B.ScopeClass && A.bAvailableInCurrentBuild == B.bAvailableInCurrentBuild &&
		   A.bPersistent == B.bPersistent && A.bHardwareBackedWhenAvailable == B.bHardwareBackedWhenAvailable &&
		   A.bAtomicCompareAndSwap == B.bAtomicCompareAndSwap && A.bAvailableInShipping == B.bAvailableInShipping;
}

bool CapabilitiesMeet(const FUnrealAISecretStoreCapabilities &Capabilities,
					  const FUnrealAIPlatformSecretStoreSelection &Selection)
{
	FString ShapeError;
	const bool bPurposeMatches =
		(Selection.Purpose == EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser &&
		 Capabilities.ProtectionClass == EUnrealAISecretStoreProtectionClass::PlatformCredentialStore &&
		 Capabilities.ScopeClass == EUnrealAISecretStoreScopeClass::CurrentUser) ||
		(Selection.Purpose == EUnrealAIPlatformSecretStorePurpose::AuthoritativeService &&
		 Capabilities.ProtectionClass == EUnrealAISecretStoreProtectionClass::ExternalSecretService &&
		 Capabilities.ScopeClass == EUnrealAISecretStoreScopeClass::Service);
	return Capabilities.ValidateShape(ShapeError) && Capabilities.bAvailableInCurrentBuild && bPurposeMatches &&
		   Capabilities.IsProductionProtected() &&
		   (!Selection.bRequirePersistent ||
			Capabilities.PersistenceClass == EUnrealAISecretStorePersistenceClass::Persistent) &&
		   (!Selection.bRequireAtomicCompareAndSwap || Capabilities.bAtomicCompareAndSwap) &&
		   (!Selection.bRequireAvailableInShipping || Capabilities.bAvailableInShipping);
}

bool EnsureSettingsCapabilityRegistration(const FName StoreName, const FUnrealAISecretStoreCapabilities &Capabilities,
										  FString &OutError)
{
	TMap<FName, FUnrealAISecretStoreCapabilities> RegisteredCapabilities;
	FUnrealAICredentialStoreRegistry::CopyCredentialStoreCapabilitiesForValidation(RegisteredCapabilities);
	if (const FUnrealAISecretStoreCapabilities *Existing = RegisteredCapabilities.Find(StoreName))
	{
		if (CapabilitiesEqual(*Existing, Capabilities))
		{
			return true;
		}
		OutError = TEXT("Platform secret-store adapter conflicts with registered settings capabilities.");
		return false;
	}
	if (FUnrealAICredentialStoreRegistry::IsSealed())
	{
		OutError = TEXT("Platform secret-store adapters must register before settings publication.");
		return false;
	}
	return FUnrealAICredentialStoreRegistry::RegisterCredentialStoreCapabilities(StoreName, Capabilities, OutError);
}

enum class ERegisteredAdapterLookup : uint8
{
	Absent,
	Selected,
	Unsupported,
	Drifted
};

ERegisteredAdapterLookup FindRegisteredAdapter(const FUnrealAIPlatformSecretStoreSelection &Selection,
											   TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> &OutStore,
											   FUnrealAISecretStoreCapabilities &OutCapabilities)
{
	OutStore.Reset();
	OutCapabilities = {};
	FPlatformSecretStoreAdapterRegistry &Registry = GetAdapterRegistry();
	TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> Candidate;
	FName RegisteredName;
	FUnrealAISecretStoreCapabilities RegisteredCapabilities;
	{
		FScopeLock Lock(&Registry.Mutex);
		const FRegisteredPlatformSecretStore *Found = Registry.Adapters.Find(Selection.Purpose);
		if (Found == nullptr)
		{
			return ERegisteredAdapterLookup::Absent;
		}
		Candidate = Found->Store;
		RegisteredName = Found->StoreName;
		RegisteredCapabilities = Found->Capabilities;
	}

	const FUnrealAISecretStoreCapabilities CurrentCapabilities = Candidate->DescribeCapabilities();
	if (Candidate->GetStoreName() != RegisteredName || !CapabilitiesEqual(CurrentCapabilities, RegisteredCapabilities))
	{
		return ERegisteredAdapterLookup::Drifted;
	}
	if (!CapabilitiesMeet(CurrentCapabilities, Selection))
	{
		return ERegisteredAdapterLookup::Unsupported;
	}
	OutStore = MoveTemp(Candidate);
	OutCapabilities = CurrentCapabilities;
	return ERegisteredAdapterLookup::Selected;
}
} // namespace

FUnrealAIPlatformSecretStoreSelection FUnrealAIPlatformSecretStoreSelection::CurrentUserApiKey(const bool bForShipping)
{
	FUnrealAIPlatformSecretStoreSelection Selection;
	Selection.Purpose = EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser;
	Selection.bRequirePersistent = true;
	Selection.bRequireAtomicCompareAndSwap = false;
	Selection.bRequireAvailableInShipping = bForShipping;
	return Selection;
}

FUnrealAIPlatformSecretStoreSelection FUnrealAIPlatformSecretStoreSelection::CurrentUserOAuth(const bool bForShipping)
{
	FUnrealAIPlatformSecretStoreSelection Selection = CurrentUserApiKey(bForShipping);
	Selection.bRequireAtomicCompareAndSwap = true;
	return Selection;
}

FUnrealAIPlatformSecretStoreSelection
FUnrealAIPlatformSecretStoreSelection::AuthoritativeService(const bool bForShipping)
{
	FUnrealAIPlatformSecretStoreSelection Selection;
	Selection.Purpose = EUnrealAIPlatformSecretStorePurpose::AuthoritativeService;
	Selection.bRequirePersistent = true;
	Selection.bRequireAtomicCompareAndSwap = true;
	Selection.bRequireAvailableInShipping = bForShipping;
	return Selection;
}

bool FUnrealAIPlatformSecretStoreSelection::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (Purpose != EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser &&
		Purpose != EUnrealAIPlatformSecretStorePurpose::AuthoritativeService)
	{
		OutError = TEXT("Platform secret-store selection requires a known deployment purpose.");
		return false;
	}
	if (Purpose == EUnrealAIPlatformSecretStorePurpose::AuthoritativeService && !bRequirePersistent)
	{
		OutError = TEXT("Authoritative service credential storage must be persistent.");
		return false;
	}
	return true;
}

bool FUnrealAIPlatformSecretStore::RegisterAdapter(const EUnrealAIPlatformSecretStorePurpose Purpose,
												   TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store,
												   FString &OutError)
{
	return RegisterAdapterInternal(Purpose, MoveTemp(Store), true, OutError);
}

#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
bool FUnrealAIPlatformSecretStore::RegisterAdapterForAutomation(
	const EUnrealAIPlatformSecretStorePurpose Purpose, TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store,
	FString &OutError)
{
	return RegisterAdapterInternal(Purpose, MoveTemp(Store), false, OutError);
}
#endif

bool FUnrealAIPlatformSecretStore::RegisterAdapterInternal(const EUnrealAIPlatformSecretStorePurpose Purpose,
														   TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store,
														   const bool bRegisterSettingsCapabilities, FString &OutError)
{
	OutError.Reset();
	const FName StoreName = Store->GetStoreName();
	const FUnrealAISecretStoreCapabilities Capabilities = Store->DescribeCapabilities();
	FUnrealAIPlatformSecretStoreSelection Baseline;
	if (Purpose == EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser)
	{
		Baseline = FUnrealAIPlatformSecretStoreSelection::CurrentUserApiKey(false);
	}
	else if (Purpose == EUnrealAIPlatformSecretStorePurpose::AuthoritativeService)
	{
		Baseline = FUnrealAIPlatformSecretStoreSelection::AuthoritativeService(false);
	}
	else
	{
		OutError = TEXT("Platform secret-store adapter requires a known deployment purpose.");
		return false;
	}
	if (StoreName.IsNone() || !CapabilitiesMeet(Capabilities, Baseline))
	{
		OutError =
			TEXT("Platform secret-store adapter has an unsafe protection, scope, persistence, or availability class.");
		return false;
	}

	FPlatformSecretStoreAdapterRegistry &Registry = GetAdapterRegistry();
	FScopeLock Lock(&Registry.Mutex);
	if (Registry.Adapters.Contains(Purpose))
	{
		OutError = TEXT("Platform secret-store purpose already has an exact registered adapter.");
		return false;
	}
	if (bRegisterSettingsCapabilities && !EnsureSettingsCapabilityRegistration(StoreName, Capabilities, OutError))
	{
		return false;
	}
	if (Store->GetStoreName() != StoreName || !CapabilitiesEqual(Store->DescribeCapabilities(), Capabilities))
	{
		OutError = TEXT("Platform secret-store adapter changed identity or capabilities during registration.");
		return false;
	}
	Registry.Adapters.Add(Purpose, FRegisteredPlatformSecretStore(MoveTemp(Store), StoreName, Capabilities));
	return true;
}

bool FUnrealAIPlatformSecretStore::UnregisterAdapter(const EUnrealAIPlatformSecretStorePurpose Purpose,
													 TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store,
													 FString &OutError)
{
	OutError.Reset();
	FPlatformSecretStoreAdapterRegistry &Registry = GetAdapterRegistry();
	FScopeLock Lock(&Registry.Mutex);
	const FRegisteredPlatformSecretStore *Found = Registry.Adapters.Find(Purpose);
	if (Found == nullptr)
	{
		OutError = TEXT("Platform secret-store purpose has no registered adapter.");
		return false;
	}
	if (&Found->Store.Get() != &Store.Get())
	{
		OutError = TEXT("Platform secret-store adapter withdrawal requires the exact registered instance.");
		return false;
	}
	Registry.Adapters.Remove(Purpose);
	return true;
}

FName FUnrealAIPlatformSecretStore::GetCompiledStoreName()
{
	return FUnrealAIMacKeychainSecretStore::IsPlatformSupported() ? FUnrealAIMacKeychainSecretStore::StoreName()
																  : NAME_None;
}

FUnrealAISecretStoreCapabilities
FUnrealAIPlatformSecretStore::DescribeCompiledCapabilities(const FUnrealAIPlatformSecretStoreSelection &Selection)
{
	FString ShapeError;
	if (!Selection.ValidateShape(ShapeError) ||
		Selection.Purpose != EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser ||
		!FUnrealAIMacKeychainSecretStore::IsPlatformSupported())
	{
		return {};
	}
	const FUnrealAIMacKeychainSecretStore Store;
	const FUnrealAISecretStoreCapabilities Capabilities = Store.DescribeCapabilities();
	return CapabilitiesMeet(Capabilities, Selection) ? Capabilities : FUnrealAISecretStoreCapabilities{};
}

FName FUnrealAIPlatformSecretStore::GetSelectedStoreName(const FUnrealAIPlatformSecretStoreSelection &Selection)
{
	TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> Store;
	FUnrealAISecretStoreCapabilities Capabilities;
	const ERegisteredAdapterLookup Lookup = FindRegisteredAdapter(Selection, Store, Capabilities);
	if (Lookup == ERegisteredAdapterLookup::Selected)
	{
		return Store->GetStoreName();
	}
	if (Lookup != ERegisteredAdapterLookup::Absent)
	{
		return NAME_None;
	}
	return DescribeCompiledCapabilities(Selection).bAvailableInCurrentBuild ? GetCompiledStoreName() : NAME_None;
}

FUnrealAISecretStoreCapabilities
FUnrealAIPlatformSecretStore::DescribeSelectedCapabilities(const FUnrealAIPlatformSecretStoreSelection &Selection)
{
	TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> Store;
	FUnrealAISecretStoreCapabilities Capabilities;
	const ERegisteredAdapterLookup Lookup = FindRegisteredAdapter(Selection, Store, Capabilities);
	if (Lookup == ERegisteredAdapterLookup::Selected)
	{
		return Capabilities;
	}
	if (Lookup != ERegisteredAdapterLookup::Absent)
	{
		return {};
	}
	return DescribeCompiledCapabilities(Selection);
}

bool FUnrealAIPlatformSecretStore::TryCreate(const FUnrealAIPlatformSecretStoreSelection &Selection,
											 TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> &OutStore,
											 FUnrealAISecretStoreCapabilities &OutCapabilities,
											 FUnrealAIProviderAccessError &OutError)
{
	OutStore.Reset();
	OutCapabilities = {};
	OutError = {};
	FString ShapeError;
	if (!Selection.ValidateShape(ShapeError))
	{
		OutError = MakeSelectionError(EUnrealAIErrorCategory::InvalidArgument,
									  EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}

	const ERegisteredAdapterLookup Lookup = FindRegisteredAdapter(Selection, OutStore, OutCapabilities);
	if (Lookup == ERegisteredAdapterLookup::Selected)
	{
		return true;
	}
	if (Lookup == ERegisteredAdapterLookup::Drifted)
	{
		OutStore.Reset();
		OutCapabilities = {};
		OutError = MakeSelectionError(EUnrealAIErrorCategory::InvalidConfiguration,
									  EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
		return false;
	}
	if (Lookup == ERegisteredAdapterLookup::Unsupported)
	{
		OutError = MakeSelectionError(EUnrealAIErrorCategory::UnsupportedCapability,
									  EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
		return false;
	}
	if (Selection.Purpose == EUnrealAIPlatformSecretStorePurpose::AuthoritativeService ||
		!FUnrealAIMacKeychainSecretStore::IsPlatformSupported())
	{
		OutError = MakeSelectionError(EUnrealAIErrorCategory::UnsupportedCapability,
									  EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
		return false;
	}

	const TSharedPtr<FUnrealAIMacKeychainSecretStore, ESPMode::ThreadSafe> Candidate =
		MakeShared<FUnrealAIMacKeychainSecretStore, ESPMode::ThreadSafe>();
	const FUnrealAISecretStoreCapabilities Capabilities = Candidate->DescribeCapabilities();
	if (!CapabilitiesMeet(Capabilities, Selection))
	{
		OutError = MakeSelectionError(EUnrealAIErrorCategory::UnsupportedCapability,
									  EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
		return false;
	}

	OutCapabilities = Capabilities;
	OutStore = Candidate;
	return true;
}
