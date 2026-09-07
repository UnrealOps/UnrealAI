// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAICredentialRefreshCoordinator.h"

#include "Misc/ScopeLock.h"

namespace
{
bool IsStableRefreshIdentifier(const FName Value)
{
	if (Value.IsNone())
	{
		return false;
	}
	const FString Text = Value.ToString();
	FTCHARToUTF8 Utf8(*Text);
	if (Text.IsEmpty() || Text.TrimStartAndEnd() != Text || Utf8.Length() <= 0 ||
		Utf8.Length() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes)
	{
		return false;
	}
	for (const TCHAR Character : Text)
	{
		if (Character < 0x21 || Character > 0x7e)
		{
			return false;
		}
	}
	return true;
}

FUnrealAIProviderAccessError MakeRefreshError(const EUnrealAIErrorCategory Category,
											  const EUnrealAIProviderAccessErrorCode Code)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	return Error;
}
} // namespace

bool FUnrealAICredentialRefreshKey::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (!IsStableRefreshIdentifier(AccountAuthProviderName) || !IsStableRefreshIdentifier(AuthProfileId) ||
		!AccountId.IsValid())
	{
		OutError = TEXT("Credential refresh key requires an exact provider, auth profile, and opaque local account.");
		return false;
	}
	return true;
}

bool FUnrealAICredentialRefreshTicket::IsValid() const
{
	FString Error;
	return (Role == EUnrealAICredentialRefreshJoinRole::Leader ||
			Role == EUnrealAICredentialRefreshJoinRole::Follower) &&
		   FlightId != 0 && AccountGeneration != 0 && Key.ValidateShape(Error);
}

bool FUnrealAICredentialRefreshTicket::IsLeader() const
{
	return Role == EUnrealAICredentialRefreshJoinRole::Leader;
}

EUnrealAICredentialRefreshJoinRole FUnrealAICredentialRefreshTicket::GetRole() const
{
	return Role;
}

uint64 FUnrealAICredentialRefreshTicket::GetFlightId() const
{
	return FlightId;
}

uint64 FUnrealAICredentialRefreshTicket::GetAccountGeneration() const
{
	return AccountGeneration;
}

struct FUnrealAICredentialRefreshCoordinator::FImpl
{
	struct FFlight final
	{
		uint64 FlightId = 0;
		uint64 AccountGeneration = 0;
		int32 JoinerCount = 0;
	};

	mutable FCriticalSection Mutex;
	TMap<FUnrealAICredentialRefreshKey, uint64> AccountGenerations;
	TMap<FUnrealAICredentialRefreshKey, FFlight> ActiveFlights;
	uint64 NextFlightId = 1;
	bool bShutdown = false;
};

FUnrealAICredentialRefreshCoordinator::FUnrealAICredentialRefreshCoordinator() : Impl(MakeUnique<FImpl>()) {}

FUnrealAICredentialRefreshCoordinator::~FUnrealAICredentialRefreshCoordinator()
{
	BeginShutdown();
}

bool FUnrealAICredentialRefreshCoordinator::BeginOrJoin(const FUnrealAICredentialRefreshKey &Key,
														FUnrealAICredentialRefreshTicket &OutTicket,
														FUnrealAIProviderAccessError &OutError)
{
	OutTicket = FUnrealAICredentialRefreshTicket{};
	OutError = FUnrealAIProviderAccessError{};
	FString ShapeError;
	if (!Key.ValidateShape(ShapeError))
	{
		OutError =
			MakeRefreshError(EUnrealAIErrorCategory::InvalidArgument, EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	FScopeLock Lock(&Impl->Mutex);
	if (Impl->bShutdown)
	{
		OutError =
			MakeRefreshError(EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::CredentialCancelled);
		return false;
	}
	if (FImpl::FFlight *Flight = Impl->ActiveFlights.Find(Key))
	{
		if (Flight->JoinerCount >= MaxJoinersPerFlight)
		{
			OutError = MakeRefreshError(EUnrealAIErrorCategory::Busy,
										EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity);
			return false;
		}
		++Flight->JoinerCount;
		OutTicket.Key = Key;
		OutTicket.Role = EUnrealAICredentialRefreshJoinRole::Follower;
		OutTicket.FlightId = Flight->FlightId;
		OutTicket.AccountGeneration = Flight->AccountGeneration;
		return true;
	}
	uint64 *Generation = Impl->AccountGenerations.Find(Key);
	if (Generation == nullptr)
	{
		if (Impl->AccountGenerations.Num() >= MaxTrackedAccounts)
		{
			OutError = MakeRefreshError(EUnrealAIErrorCategory::Busy,
										EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity);
			return false;
		}
		Generation = &Impl->AccountGenerations.Add(Key, 1);
	}
	if (Impl->NextFlightId == 0 || *Generation == 0)
	{
		Impl->bShutdown = true;
		Impl->ActiveFlights.Reset();
		OutError = MakeRefreshError(EUnrealAIErrorCategory::Internal, EUnrealAIProviderAccessErrorCode::Internal);
		return false;
	}
	FImpl::FFlight Flight;
	Flight.FlightId = Impl->NextFlightId++;
	Flight.AccountGeneration = *Generation;
	Flight.JoinerCount = 1;
	Impl->ActiveFlights.Add(Key, Flight);
	OutTicket.Key = Key;
	OutTicket.Role = EUnrealAICredentialRefreshJoinRole::Leader;
	OutTicket.FlightId = Flight.FlightId;
	OutTicket.AccountGeneration = Flight.AccountGeneration;
	return true;
}

bool FUnrealAICredentialRefreshCoordinator::Complete(const FUnrealAICredentialRefreshTicket &LeaderTicket)
{
	if (!LeaderTicket.IsValid() || !LeaderTicket.IsLeader())
	{
		return false;
	}
	FScopeLock Lock(&Impl->Mutex);
	if (Impl->bShutdown)
	{
		return false;
	}
	const FImpl::FFlight *Flight = Impl->ActiveFlights.Find(LeaderTicket.Key);
	if (Flight == nullptr || Flight->FlightId != LeaderTicket.FlightId ||
		Flight->AccountGeneration != LeaderTicket.AccountGeneration)
	{
		return false;
	}
	Impl->ActiveFlights.Remove(LeaderTicket.Key);
	return true;
}

bool FUnrealAICredentialRefreshCoordinator::IsCurrent(const FUnrealAICredentialRefreshTicket &Ticket) const
{
	if (!Ticket.IsValid())
	{
		return false;
	}
	FScopeLock Lock(&Impl->Mutex);
	if (Impl->bShutdown)
	{
		return false;
	}
	const FImpl::FFlight *Flight = Impl->ActiveFlights.Find(Ticket.Key);
	return Flight != nullptr && Flight->FlightId == Ticket.FlightId &&
		   Flight->AccountGeneration == Ticket.AccountGeneration;
}

bool FUnrealAICredentialRefreshCoordinator::Invalidate(const FUnrealAICredentialRefreshKey &Key)
{
	FString ShapeError;
	if (!Key.ValidateShape(ShapeError))
	{
		return false;
	}
	FScopeLock Lock(&Impl->Mutex);
	if (Impl->bShutdown)
	{
		return false;
	}
	uint64 *Generation = Impl->AccountGenerations.Find(Key);
	if (Generation == nullptr)
	{
		return false;
	}
	Impl->ActiveFlights.Remove(Key);
	if (*Generation == TNumericLimits<uint64>::Max())
	{
		Impl->bShutdown = true;
		Impl->ActiveFlights.Reset();
		return false;
	}
	++*Generation;
	return true;
}

void FUnrealAICredentialRefreshCoordinator::BeginShutdown()
{
	FScopeLock Lock(&Impl->Mutex);
	Impl->bShutdown = true;
	Impl->ActiveFlights.Reset();
}

bool FUnrealAICredentialRefreshCoordinator::IsShutdown() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->bShutdown;
}

int32 FUnrealAICredentialRefreshCoordinator::NumActiveFlights() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->ActiveFlights.Num();
}

int32 FUnrealAICredentialRefreshCoordinator::NumTrackedAccounts() const
{
	FScopeLock Lock(&Impl->Mutex);
	return Impl->AccountGenerations.Num();
}
