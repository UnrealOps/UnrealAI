// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIEndpointProfileRegistry.h"

#include "Misc/ScopeLock.h"

namespace
{
bool ProfileLess(const TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &A,
				 const TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &B)
{
	return A->GetProfileId().ToString() < B->GetProfileId().ToString();
}
} // namespace

struct FUnrealAIEndpointProfileRegistrySnapshot::FImpl
{
	TMap<FName, TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>> Entries;
};

struct FUnrealAIEndpointProfileRegistry::FImpl
{
	mutable FCriticalSection Mutex;
	TMap<FName, TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>> Entries;
	uint64 Generation = 0;
};

FUnrealAIEndpointProfileRegistrySnapshot::FUnrealAIEndpointProfileRegistrySnapshot(
	TSharedRef<const FImpl, ESPMode::ThreadSafe> InImpl, const uint64 InGeneration)
	: Impl(MoveTemp(InImpl)), Generation(InGeneration)
{
}

TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>
FUnrealAIEndpointProfileRegistrySnapshot::Find(const FName ProfileId) const
{
	const TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> *Found =
		Impl->Entries.Find(ProfileId);
	return Found == nullptr ? nullptr : Found->ToSharedPtr();
}

TArray<TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>>
FUnrealAIEndpointProfileRegistrySnapshot::GetProfiles() const
{
	TArray<TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>> Result;
	Result.Reserve(Impl->Entries.Num());
	for (const TPair<FName, TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>> &Pair :
		 Impl->Entries)
	{
		Result.Add(Pair.Value);
	}
	Result.Sort(ProfileLess);
	return Result;
}

int32 FUnrealAIEndpointProfileRegistrySnapshot::Num() const
{
	return Impl->Entries.Num();
}

FUnrealAIEndpointProfileRegistry::FUnrealAIEndpointProfileRegistry() : Impl(MakeUnique<FImpl>()) {}

FUnrealAIEndpointProfileRegistry::~FUnrealAIEndpointProfileRegistry() = default;

bool FUnrealAIEndpointProfileRegistry::RegisterLocalInProcessEndpoint(
	const FName ProfileId, const FName ModelProviderName, const uint64 PolicyRevision,
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered, FString &OutError)
{
	FUnrealAIEndpointProfileDescriptor Descriptor;
	Descriptor.ProfileId = ProfileId;
	Descriptor.ModelProviderName = ModelProviderName;
	Descriptor.ProfileClass = EUnrealAIEndpointProfileClass::LocalInProcess;
	Descriptor.AuthScheme = EUnrealAIAuthScheme::Anonymous;
	Descriptor.BillingMode = EUnrealAIBillingMode::Local;
	Descriptor.PolicyRevision = PolicyRevision;
	return RegisterNormalized(MoveTemp(Descriptor), OutRegistered, OutError);
}

bool FUnrealAIEndpointProfileRegistry::RegisterCustomApiEndpoint(
	const FName ProfileId, const FName ModelProviderName, const FUnrealAIEndpointOrigin &Origin,
	const FString &Audience, const uint64 PolicyRevision,
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered, FString &OutError)
{
	FUnrealAIEndpointProfileDescriptor Descriptor;
	Descriptor.ProfileId = ProfileId;
	Descriptor.ModelProviderName = ModelProviderName;
	Descriptor.ProfileClass = Origin.IsLoopbackDevelopmentOnly()
								  ? EUnrealAIEndpointProfileClass::LocalLoopbackDevelopment
								  : EUnrealAIEndpointProfileClass::CustomApi;
	Descriptor.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Descriptor.BillingMode = EUnrealAIBillingMode::ApiMetered;
	Descriptor.Origin = Origin;
	Descriptor.Audience = Audience;
	Descriptor.PolicyRevision = PolicyRevision;
	if (!IsProfileClassAllowedInBuild(Descriptor.ProfileClass, UE_BUILD_SHIPPING != 0))
	{
		OutRegistered.Reset();
		OutError = TEXT("Shipping builds reject loopback plaintext endpoint profiles.");
		return false;
	}
	return RegisterNormalized(MoveTemp(Descriptor), OutRegistered, OutError);
}

bool FUnrealAIEndpointProfileRegistry::RegisterProjectGatewayEndpoint(
	const FName ProfileId, const FName ModelProviderName, const FUnrealAIEndpointOrigin &Origin,
	const FString &Audience, const uint64 PolicyRevision,
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered, FString &OutError)
{
	FUnrealAIEndpointProfileDescriptor Descriptor;
	Descriptor.ProfileId = ProfileId;
	Descriptor.ModelProviderName = ModelProviderName;
	Descriptor.ProfileClass = EUnrealAIEndpointProfileClass::ProjectGateway;
	Descriptor.AuthScheme = EUnrealAIAuthScheme::GatewayBearer;
	Descriptor.BillingMode = EUnrealAIBillingMode::GatewayAccounted;
	Descriptor.Origin = Origin;
	Descriptor.Audience = Audience;
	Descriptor.PolicyRevision = PolicyRevision;
	return RegisterNormalized(MoveTemp(Descriptor), OutRegistered, OutError);
}

bool FUnrealAIEndpointProfileRegistry::RegisterProviderSubscriptionEndpoint(
	const FUnrealAIProviderEndpointAuthority &Authority, const FName ProfileId, const FName ModelProviderName,
	const FUnrealAIEndpointOrigin &Origin, const FString &Audience, const uint64 PolicyRevision,
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered, FString &OutError)
{
	OutRegistered.Reset();
	OutError.Reset();
	if (!Authority.Authorizes(ModelProviderName, EUnrealAIAuthScheme::OAuthBearer,
							  EUnrealAIBillingMode::SubscriptionQuota))
	{
		OutError = TEXT("Subscription endpoint authority does not authorize the requested model provider.");
		return false;
	}
	FUnrealAIEndpointProfileDescriptor Descriptor;
	Descriptor.ProfileId = ProfileId;
	Descriptor.ModelProviderName = ModelProviderName;
	Descriptor.AuthProviderName = Authority.GetAuthProviderName();
	Descriptor.ProfileClass = EUnrealAIEndpointProfileClass::ProviderSubscriptionResource;
	Descriptor.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Descriptor.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Descriptor.Origin = Origin;
	Descriptor.Audience = Audience;
	Descriptor.PolicyRevision = PolicyRevision;
	return RegisterNormalized(MoveTemp(Descriptor), OutRegistered, OutError);
}

bool FUnrealAIEndpointProfileRegistry::RegisterNormalized(
	FUnrealAIEndpointProfileDescriptor &&Descriptor,
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> &OutRegistered, FString &OutError)
{
	OutRegistered.Reset();
	OutError.Reset();
	if (!Descriptor.ValidateShape(OutError))
	{
		return false;
	}
	FScopeLock Lock(&Impl->Mutex);
	if (Impl->Entries.Contains(Descriptor.ProfileId))
	{
		OutError = TEXT("Endpoint profile registry rejected a duplicate profile ID.");
		return false;
	}
	if (Impl->Entries.Num() >= MaxProfiles)
	{
		OutError = TEXT("Endpoint profile registry reached its bounded capacity.");
		return false;
	}
	const FName ProfileId = Descriptor.ProfileId;
	const TSharedRef<FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Mutable =
		MakeShared<FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>(MoveTemp(Descriptor));
	const TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Published = Mutable;
	Impl->Entries.Add(ProfileId, Published);
	++Impl->Generation;
	OutRegistered = Published.ToSharedPtr();
	return true;
}

TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe>
FUnrealAIEndpointProfileRegistry::Find(const FName ProfileId) const
{
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> *Found =
		Impl->Entries.Find(ProfileId);
	return Found == nullptr ? nullptr : Found->ToSharedPtr();
}

TSharedRef<const FUnrealAIEndpointProfileRegistrySnapshot, ESPMode::ThreadSafe>
FUnrealAIEndpointProfileRegistry::CreateSnapshot() const
{
	FScopeLock Lock(&Impl->Mutex);
	const TSharedRef<FUnrealAIEndpointProfileRegistrySnapshot::FImpl, ESPMode::ThreadSafe> SnapshotImpl =
		MakeShared<FUnrealAIEndpointProfileRegistrySnapshot::FImpl, ESPMode::ThreadSafe>();
	SnapshotImpl->Entries = Impl->Entries;
	return MakeShareable(new FUnrealAIEndpointProfileRegistrySnapshot(SnapshotImpl, Impl->Generation));
}

int32 FUnrealAIEndpointProfileRegistry::Num() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Entries.Num();
}

uint64 FUnrealAIEndpointProfileRegistry::GetGeneration() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->Generation;
}

bool FUnrealAIEndpointProfileRegistry::IsProfileClassAllowedInBuild(const EUnrealAIEndpointProfileClass ProfileClass,
																	const bool bShippingBuild)
{
	return !bShippingBuild || ProfileClass != EUnrealAIEndpointProfileClass::LocalLoopbackDevelopment;
}
