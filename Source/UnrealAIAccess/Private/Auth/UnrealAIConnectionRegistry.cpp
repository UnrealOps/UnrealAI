// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIConnectionRegistry.h"

#include "Misc/ScopeLock.h"

namespace
{
bool DescriptorEqual(const FUnrealAIConnectionDescriptor &A, const FUnrealAIConnectionDescriptor &B)
{
	const FUnrealAICredentialDestination &AccessA = A.CredentialDestination;
	const FUnrealAICredentialDestination &AccessB = B.CredentialDestination;
	return A.SchemaVersion == B.SchemaVersion && A.ConnectionRevision == B.ConnectionRevision &&
		   A.ConnectionAlias == B.ConnectionAlias && A.EndpointProfileId == B.EndpointProfileId &&
		   AccessA.ModelProviderName == AccessB.ModelProviderName &&
		   AccessA.AccountAuthProviderName == AccessB.AccountAuthProviderName &&
		   AccessA.AuthProfileId == AccessB.AuthProfileId && AccessA.AccountId == AccessB.AccountId &&
		   AccessA.TenantRealm == AccessB.TenantRealm && AccessA.BillingPrincipalId == AccessB.BillingPrincipalId &&
		   AccessA.PayerHandle == AccessB.PayerHandle && AccessA.AuthScheme == AccessB.AuthScheme &&
		   AccessA.BillingMode == AccessB.BillingMode && AccessA.EndpointOrigin == AccessB.EndpointOrigin &&
		   AccessA.Audience == AccessB.Audience && AccessA.ConnectionRevision == AccessB.ConnectionRevision &&
		   AccessA.EndpointPolicyRevision == AccessB.EndpointPolicyRevision;
}

bool FallbackDescriptorEqual(const FUnrealAIConnectionDescriptor &A, const FUnrealAIConnectionDescriptor &B)
{
	const FUnrealAICredentialDestination &AccessA = A.CredentialDestination;
	const FUnrealAICredentialDestination &AccessB = B.CredentialDestination;
	return A.SchemaVersion == B.SchemaVersion && A.ConnectionRevision == B.ConnectionRevision &&
		   A.EndpointProfileId == B.EndpointProfileId && AccessA.ModelProviderName == AccessB.ModelProviderName &&
		   AccessA.AccountAuthProviderName == AccessB.AccountAuthProviderName &&
		   AccessA.AuthProfileId == AccessB.AuthProfileId && AccessA.AccountId == AccessB.AccountId &&
		   AccessA.TenantRealm == AccessB.TenantRealm && AccessA.BillingPrincipalId == AccessB.BillingPrincipalId &&
		   AccessA.PayerHandle == AccessB.PayerHandle && AccessA.AuthScheme == AccessB.AuthScheme &&
		   AccessA.BillingMode == AccessB.BillingMode && AccessA.EndpointOrigin == AccessB.EndpointOrigin &&
		   AccessA.Audience == AccessB.Audience && AccessA.ConnectionRevision == AccessB.ConnectionRevision &&
		   AccessA.EndpointPolicyRevision == AccessB.EndpointPolicyRevision;
}

bool ConnectionLess(const TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> &A,
					const TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> &B)
{
	return A->ConnectionAlias.ToString() < B->ConnectionAlias.ToString();
}
} // namespace

struct FUnrealAIConnectionRegistrySnapshot::FImpl
{
	TMap<FName, TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>> Entries;
};

struct FUnrealAIConnectionRegistry::FImpl
{
	explicit FImpl(TSharedRef<const FUnrealAIEndpointProfileRegistrySnapshot, ESPMode::ThreadSafe> InEndpointProfiles)
		: EndpointProfiles(MoveTemp(InEndpointProfiles))
	{
	}

	mutable FCriticalSection Mutex;
	TSharedRef<const FUnrealAIEndpointProfileRegistrySnapshot, ESPMode::ThreadSafe> EndpointProfiles;
	TMap<FName, TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>> Entries;
	uint64 Generation = 0;
};

FUnrealAIConnectionRegistrySnapshot::FUnrealAIConnectionRegistrySnapshot(
	TSharedRef<const FImpl, ESPMode::ThreadSafe> InImpl, const uint64 InGeneration)
	: Impl(MoveTemp(InImpl)), Generation(InGeneration)
{
}

TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>
FUnrealAIConnectionRegistrySnapshot::Find(const FName ConnectionAlias) const
{
	const TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> *Found =
		Impl->Entries.Find(ConnectionAlias);
	return Found == nullptr ? nullptr : Found->ToSharedPtr();
}

TArray<TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>>
FUnrealAIConnectionRegistrySnapshot::GetConnections() const
{
	TArray<TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>> Result;
	Result.Reserve(Impl->Entries.Num());
	for (const TPair<FName, TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>> &Pair : Impl->Entries)
	{
		Result.Add(Pair.Value);
	}
	Result.Sort(ConnectionLess);
	return Result;
}

int32 FUnrealAIConnectionRegistrySnapshot::Num() const
{
	return Impl->Entries.Num();
}

FUnrealAIConnectionRegistry::FUnrealAIConnectionRegistry(
	TSharedRef<const FUnrealAIEndpointProfileRegistrySnapshot, ESPMode::ThreadSafe> InEndpointProfiles)
	: Impl(MakeUnique<FImpl>(MoveTemp(InEndpointProfiles)))
{
}

FUnrealAIConnectionRegistry::~FUnrealAIConnectionRegistry() = default;

bool FUnrealAIConnectionRegistry::Register(
	const FUnrealAIConnectionDescriptor &Descriptor,
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> &OutRegistered, FString &OutError)
{
	OutRegistered.Reset();
	OutError.Reset();
	if (!Descriptor.ValidateShape(OutError))
	{
		return false;
	}
	const TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> EndpointProfile =
		Impl->EndpointProfiles->Find(Descriptor.EndpointProfileId);
	if (!EndpointProfile.IsValid())
	{
		OutError = TEXT("Connection registry rejected an unknown authoritative endpoint profile.");
		return false;
	}
	if (!EndpointProfile->MatchesDestination(Descriptor.CredentialDestination))
	{
		OutError = TEXT("Connection registry rejected destination drift from its authoritative endpoint profile.");
		return false;
	}
	if (Descriptor.IsSubscriptionConnection() &&
		EndpointProfile->GetProfileClass() != EUnrealAIEndpointProfileClass::ProviderSubscriptionResource)
	{
		OutError = TEXT("Connection registry rejected subscription access without provider endpoint authority.");
		return false;
	}
	if (!FUnrealAIEndpointProfileRegistry::IsProfileClassAllowedInBuild(EndpointProfile->GetProfileClass(),
																		UE_BUILD_SHIPPING != 0))
	{
		OutError = TEXT("Shipping connection registry rejected a development-only plaintext endpoint.");
		return false;
	}

	FScopeLock Lock(&Impl->Mutex);
	if (const TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> *Existing =
			Impl->Entries.Find(Descriptor.ConnectionAlias))
	{
		OutError = DescriptorEqual(Existing->Get(), Descriptor)
			? TEXT("Connection registry rejected a duplicate alias registration.")
			: TEXT("Connection registry rejected alias drift without a new alias.");
		return false;
	}
	if (Impl->Entries.Num() >= MaxConnections)
	{
		OutError = TEXT("Connection registry reached its bounded capacity.");
		return false;
	}

	const TSharedRef<FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Mutable =
		MakeShared<FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>(Descriptor);
	const TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Published = Mutable;
	Impl->Entries.Add(Descriptor.ConnectionAlias, Published);
	++Impl->Generation;
	OutRegistered = Published.ToSharedPtr();
	return true;
}

TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe>
FUnrealAIConnectionRegistry::Find(const FName ConnectionAlias) const
{
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> *Found =
		Impl->Entries.Find(ConnectionAlias);
	return Found == nullptr ? nullptr : Found->ToSharedPtr();
}

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe>
FUnrealAIConnectionRegistry::CreateSnapshot() const
{
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<FUnrealAIConnectionRegistrySnapshot::FImpl, ESPMode::ThreadSafe> SnapshotImpl =
		MakeShared<FUnrealAIConnectionRegistrySnapshot::FImpl, ESPMode::ThreadSafe>();
	SnapshotImpl->Entries = Impl->Entries;
	return MakeShareable(new FUnrealAIConnectionRegistrySnapshot(SnapshotImpl, Impl->Generation));
}

int32 FUnrealAIConnectionRegistry::Num() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Entries.Num();
}

uint64 FUnrealAIConnectionRegistry::GetGeneration() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Generation;
}

bool FUnrealAIConnectionRegistry::CanFallbackWithoutPayerChange(const FUnrealAIConnectionDescriptor &Primary,
																const FUnrealAIConnectionDescriptor &Candidate,
																FString &OutError)
{
	OutError.Reset();
	FString ShapeError;
	if (!Primary.ValidateShape(ShapeError) || !Candidate.ValidateShape(ShapeError))
	{
		OutError = TEXT("Fallback comparison requires two valid connection descriptors.");
		return false;
	}
	if (!FallbackDescriptorEqual(Primary, Candidate))
	{
		OutError =
			TEXT("Automatic fallback cannot change schema, connection revision, endpoint profile, provider, origin, "
						"audience, endpoint policy, auth scheme, auth profile, account, tenant, billing principal, payer, "
						"or billing mode.");
		return false;
	}
	return true;
}
