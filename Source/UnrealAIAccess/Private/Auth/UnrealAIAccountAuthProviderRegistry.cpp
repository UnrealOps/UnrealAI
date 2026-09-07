// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIAccountAuthProviderRegistry.h"

#include "Misc/ScopeLock.h"

namespace
{
bool IsStableProviderName(const FName Name)
{
	if (Name.IsNone())
	{
		return false;
	}
	const FString Text = Name.ToString();
	if (Text.IsEmpty() || Text.Len() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes)
	{
		return false;
	}
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Character = Text[Index];
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9'));
		if ((!bAlphaNumeric && Character != TEXT('.') && Character != TEXT('_') && Character != TEXT('-')) ||
															 ((Index == 0 || Index == Text.Len() - 1) &&
															  !bAlphaNumeric))
		{
			return false;
		}
	}
	return true;
}
} // namespace

struct FUnrealAIAccountAuthProviderRegistrySnapshot::FImpl
{
	TMap<FName, TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe>> Providers;
};

struct FUnrealAIAccountAuthProviderRegistry::FImpl
{
	mutable FCriticalSection Mutex;
	TMap<FName, TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe>> Providers;
	uint64 Generation = 0;
};

FUnrealAIAccountAuthProviderRegistrySnapshot::FUnrealAIAccountAuthProviderRegistrySnapshot(
	TSharedRef<const FImpl, ESPMode::ThreadSafe> InImpl, const uint64 InGeneration)
	: Impl(MoveTemp(InImpl)), Generation(InGeneration)
{
}

TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe>
FUnrealAIAccountAuthProviderRegistrySnapshot::Find(const FName ProviderName) const
{
	const TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> *Found = Impl->Providers.Find(ProviderName);
	return Found == nullptr ? nullptr : Found->ToSharedPtr();
}

TArray<FName> FUnrealAIAccountAuthProviderRegistrySnapshot::GetProviderNames() const
{
	TArray<FName> Names;
	Impl->Providers.GetKeys(Names);
	Names.Sort(FNameLexicalLess());
	return Names;
}

int32 FUnrealAIAccountAuthProviderRegistrySnapshot::Num() const
{
	return Impl->Providers.Num();
}

FUnrealAIAccountAuthProviderRegistry::FUnrealAIAccountAuthProviderRegistry() : Impl(MakeUnique<FImpl>()) {}

FUnrealAIAccountAuthProviderRegistry::~FUnrealAIAccountAuthProviderRegistry() = default;

bool FUnrealAIAccountAuthProviderRegistry::Register(
	TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider,
	FUnrealAIProviderEndpointAuthority &OutAuthority, FString &OutError)
{
	OutAuthority = FUnrealAIProviderEndpointAuthority{};
	OutError.Reset();
	const FName ProviderName = Provider->GetProviderName();
	const FUnrealAIProviderAccessDescriptor Access = Provider->DescribeAccess();
	const FUnrealAIAccountAuthCapabilities Capabilities = Provider->DescribeCapabilities();
	FString AccessError;
	if (!IsStableProviderName(ProviderName) || !Access.ValidateShape(AccessError) ||
		Access.AccountAuthProviderName != ProviderName)
	{
		OutError = TEXT("Account-auth provider registry rejected an invalid provider identity or access descriptor.");
		return false;
	}
	const bool bMustAdvertiseNoInteractiveAccess =
		Access.Availability == EUnrealAIProviderAccessAvailability::PartnerGated ||
		Access.Availability == EUnrealAIProviderAccessAvailability::Unsupported;
	if (bMustAdvertiseNoInteractiveAccess &&
		(Capabilities.bBrowserPkce || Capabilities.bDeviceCode || Capabilities.bRefresh || Capabilities.bRevocation))
	{
		OutError = TEXT("Gated account-auth providers cannot advertise usable authorization capabilities.");
		return false;
	}
	FScopeLock Lock(&Impl->Mutex);
	if (Impl->Providers.Contains(ProviderName))
	{
		OutError = TEXT("Account-auth provider registry rejected a duplicate provider name.");
		return false;
	}
	if (Impl->Providers.Num() >= MaxProviders)
	{
		OutError = TEXT("Account-auth provider registry reached its bounded capacity.");
		return false;
	}
	Impl->Providers.Add(ProviderName, Provider);
	++Impl->Generation;
	const bool bApprovedSubscriptionResource =
		Access.AuthScheme == EUnrealAIAuthScheme::OAuthBearer &&
		Access.BillingMode == EUnrealAIBillingMode::SubscriptionQuota &&
		(Access.Availability == EUnrealAIProviderAccessAvailability::Available ||
		 Access.Availability == EUnrealAIProviderAccessAvailability::SignedOut ||
		 Access.Availability == EUnrealAIProviderAccessAvailability::ReauthenticationRequired);
	if (bApprovedSubscriptionResource)
	{
		OutAuthority = FUnrealAIProviderEndpointAuthority(ProviderName, Access.ModelProviderName, Access.AuthScheme,
														  Access.BillingMode);
	}
	return true;
}

bool FUnrealAIAccountAuthProviderRegistry::Unregister(
	TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> Provider, FString &OutError)
{
	OutError.Reset();
	const FName ProviderName = Provider->GetProviderName();
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> *Registered =
		Impl->Providers.Find(ProviderName);
	if (Registered == nullptr)
	{
		OutError = TEXT("Account-auth provider registry cannot unregister an absent provider.");
		return false;
	}
	if (&Registered->Get() != &Provider.Get())
	{
		OutError = TEXT("Account-auth provider registry rejected removal of a different provider instance.");
		return false;
	}
	Impl->Providers.Remove(ProviderName);
	++Impl->Generation;
	return true;
}

TSharedPtr<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe>
FUnrealAIAccountAuthProviderRegistry::Find(const FName ProviderName) const
{
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<IUnrealAIAccountAuthProvider, ESPMode::ThreadSafe> *Found = Impl->Providers.Find(ProviderName);
	return Found == nullptr ? nullptr : Found->ToSharedPtr();
}

TSharedRef<const FUnrealAIAccountAuthProviderRegistrySnapshot, ESPMode::ThreadSafe>
FUnrealAIAccountAuthProviderRegistry::CreateSnapshot() const
{
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<FUnrealAIAccountAuthProviderRegistrySnapshot::FImpl, ESPMode::ThreadSafe> SnapshotImpl =
		MakeShared<FUnrealAIAccountAuthProviderRegistrySnapshot::FImpl, ESPMode::ThreadSafe>();
	SnapshotImpl->Providers = Impl->Providers;
	return MakeShareable(new FUnrealAIAccountAuthProviderRegistrySnapshot(SnapshotImpl, Impl->Generation));
}

int32 FUnrealAIAccountAuthProviderRegistry::Num() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Providers.Num();
}

uint64 FUnrealAIAccountAuthProviderRegistry::GetGeneration() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Generation;
}
