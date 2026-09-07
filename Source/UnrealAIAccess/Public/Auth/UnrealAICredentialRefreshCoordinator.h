// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

/** Exact local account key for one refresh single-flight domain. */
struct UNREALAIACCESS_API FUnrealAICredentialRefreshKey final
{
	FName AccountAuthProviderName;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;

	bool ValidateShape(FString &OutError) const;

	friend bool operator==(const FUnrealAICredentialRefreshKey &A, const FUnrealAICredentialRefreshKey &B)
	{
		return A.AccountAuthProviderName == B.AccountAuthProviderName && A.AuthProfileId == B.AuthProfileId &&
			   A.AccountId == B.AccountId;
	}
	friend uint32 GetTypeHash(const FUnrealAICredentialRefreshKey &Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.AccountAuthProviderName), GetTypeHash(Key.AuthProfileId)),
						   GetTypeHash(Key.AccountId));
	}
};

enum class EUnrealAICredentialRefreshJoinRole : uint8
{
	Invalid,
	Leader,
	Follower
};

/** Non-secret admission ticket. Only the leader may settle its exact flight. */
class UNREALAIACCESS_API FUnrealAICredentialRefreshTicket final
{
  public:
	bool IsValid() const;
	bool IsLeader() const;
	EUnrealAICredentialRefreshJoinRole GetRole() const;
	uint64 GetFlightId() const;
	uint64 GetAccountGeneration() const;

  private:
	FUnrealAICredentialRefreshKey Key;
	EUnrealAICredentialRefreshJoinRole Role = EUnrealAICredentialRefreshJoinRole::Invalid;
	uint64 FlightId = 0;
	uint64 AccountGeneration = 0;

	friend class FUnrealAICredentialRefreshCoordinator;
};

/**
 * Thread-safe broker primitive that coalesces concurrent refresh demand per exact local account. It owns no tokens or
 * callbacks. Invalidation/shutdown makes every outstanding ticket stale before a late provider result can settle.
 */
class UNREALAIACCESS_API FUnrealAICredentialRefreshCoordinator final
{
  public:
	static constexpr int32 MaxTrackedAccounts = 256;
	static constexpr int32 MaxJoinersPerFlight = 1024;

	FUnrealAICredentialRefreshCoordinator();
	~FUnrealAICredentialRefreshCoordinator();
	FUnrealAICredentialRefreshCoordinator(const FUnrealAICredentialRefreshCoordinator &) = delete;
	FUnrealAICredentialRefreshCoordinator &operator=(const FUnrealAICredentialRefreshCoordinator &) = delete;

	bool BeginOrJoin(const FUnrealAICredentialRefreshKey &Key, FUnrealAICredentialRefreshTicket &OutTicket,
					 FUnrealAIProviderAccessError &OutError);
	bool Complete(const FUnrealAICredentialRefreshTicket &LeaderTicket);
	bool IsCurrent(const FUnrealAICredentialRefreshTicket &Ticket) const;
	bool Invalidate(const FUnrealAICredentialRefreshKey &Key);
	void BeginShutdown();
	bool IsShutdown() const;
	int32 NumActiveFlights() const;
	int32 NumTrackedAccounts() const;

  private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
