// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAICredentialStoreRegistry.h"
#include "Misc/ScopeLock.h"

namespace
{
struct FCredentialStoreRegistryState
{
	FCriticalSection Mutex;
	TMap<FName, FUnrealAISecretStoreCapabilities> Stores;
	bool bSealed = false;
};

FCredentialStoreRegistryState &GetCredentialStoreRegistry()
{
	static FCredentialStoreRegistryState State;
	return State;
}

bool EqualStoreCapabilities(const FUnrealAISecretStoreCapabilities &A, const FUnrealAISecretStoreCapabilities &B)
{
	return A.PersistenceClass == B.PersistenceClass && A.ProtectionClass == B.ProtectionClass &&
		   A.ScopeClass == B.ScopeClass && A.bAvailableInCurrentBuild == B.bAvailableInCurrentBuild &&
		   A.bPersistent == B.bPersistent && A.bHardwareBackedWhenAvailable == B.bHardwareBackedWhenAvailable &&
		   A.bAtomicCompareAndSwap == B.bAtomicCompareAndSwap && A.bAvailableInShipping == B.bAvailableInShipping;
}
} // namespace

bool FUnrealAICredentialStoreRegistry::RegisterCredentialStoreCapabilities(
	FName StoreAlias, const FUnrealAISecretStoreCapabilities &Capabilities, FString &OutError)
{
	OutError.Reset();
	const FString Name = StoreAlias.ToString();
	FString ShapeError;
	if (StoreAlias.IsNone() || Name.Len() > 128 || !Capabilities.ValidateShape(ShapeError))
	{
		OutError = TEXT("The credential-store capability registration is invalid.");
		return false;
	}
	for (TCHAR Character : Name)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('.') && Character != TEXT('_') && Character != TEXT('-'))
		{
			OutError = TEXT("The credential-store alias is invalid.");
			return false;
		}
	}
	FCredentialStoreRegistryState &State = GetCredentialStoreRegistry();
	FScopeLock Lock(&State.Mutex);
	if (State.bSealed)
	{
		OutError = TEXT("Credential-store capability registration is closed after publication.");
		return false;
	}
	if (const FUnrealAISecretStoreCapabilities *Existing = State.Stores.Find(StoreAlias))
	{
		if (EqualStoreCapabilities(*Existing, Capabilities))
		{
			return true;
		}
		OutError = TEXT("A credential-store alias has conflicting capability metadata.");
		return false;
	}
	if (State.Stores.Num() >= MaxCredentialStores)
	{
		OutError = TEXT("The credential-store capability registry reached its bounded capacity.");
		return false;
	}
	State.Stores.Add(StoreAlias, Capabilities);
	return true;
}

void FUnrealAICredentialStoreRegistry::CopyCredentialStoreCapabilitiesForValidation(
	TMap<FName, FUnrealAISecretStoreCapabilities> &OutCapabilities)
{
	FCredentialStoreRegistryState &State = GetCredentialStoreRegistry();
	FScopeLock Lock(&State.Mutex);
	OutCapabilities = State.Stores;
}

void FUnrealAICredentialStoreRegistry::Seal()
{
	FCredentialStoreRegistryState &State = GetCredentialStoreRegistry();
	FScopeLock Lock(&State.Mutex);
	State.bSealed = true;
}

bool FUnrealAICredentialStoreRegistry::IsSealed()
{
	FCredentialStoreRegistryState &State = GetCredentialStoreRegistry();
	FScopeLock Lock(&State.Mutex);
	return State.bSealed;
}
