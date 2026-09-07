// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Async/ParallelFor.h"
#include "Auth/UnrealAICredentialRefreshCoordinator.h"

namespace
{
FUnrealAICredentialRefreshKey MakeRefreshKey(const uint32 Suffix)
{
	FUnrealAICredentialRefreshKey Key;
	Key.AccountAuthProviderName = TEXT("tests.account_auth");
	Key.AuthProfileId = TEXT("tests.profile");
	Key.AccountId.Value = FGuid(100, 200, 300, Suffix + 1);
	return Key;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAICredentialRefreshCoordinatorTest,
								 "UnrealAI.Auth.CredentialRefreshSingleFlightInvalidationAndShutdown",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAICredentialRefreshCoordinatorTest::RunTest(const FString &Parameters)
{
	FUnrealAICredentialRefreshCoordinator Coordinator;
	const FUnrealAICredentialRefreshKey Key = MakeRefreshKey(0);
	constexpr int32 JoinerCount = 64;
	TArray<FUnrealAICredentialRefreshTicket> Tickets;
	Tickets.SetNum(JoinerCount);
	TAtomic<int32> Accepted{0};
	ParallelFor(JoinerCount,
				[&Coordinator, &Key, &Tickets, &Accepted](const int32 Index)
				{
					FUnrealAIProviderAccessError Error;
					if (Coordinator.BeginOrJoin(Key, Tickets[Index], Error))
					{
						++Accepted;
					}
				});
	TestEqual(TEXT("Every bounded concurrent refresh request joins"), Accepted.Load(), JoinerCount);
	int32 Leaders = 0;
	uint64 FlightId = 0;
	int32 LeaderIndex = INDEX_NONE;
	bool bOneSharedFlight = true;
	for (int32 Index = 0; Index < Tickets.Num(); ++Index)
	{
		const FUnrealAICredentialRefreshTicket &Ticket = Tickets[Index];
		if (Ticket.IsLeader())
		{
			++Leaders;
			LeaderIndex = Index;
		}
		if (FlightId == 0)
		{
			FlightId = Ticket.GetFlightId();
		}
		bOneSharedFlight &= Ticket.IsValid() && Ticket.GetFlightId() == FlightId && Coordinator.IsCurrent(Ticket);
	}
	TestEqual(TEXT("Concurrent refresh demand elects exactly one leader"), Leaders, 1);
	TestTrue(TEXT("Every follower is bound to the same current refresh flight"), bOneSharedFlight);
	TestEqual(TEXT("Single-flight coordinator owns one active provider refresh"), Coordinator.NumActiveFlights(), 1);
	const int32 FollowerIndex = LeaderIndex == 0 ? 1 : 0;
	TestFalse(TEXT("A follower cannot settle the provider refresh"), Coordinator.Complete(Tickets[FollowerIndex]));

	TestTrue(TEXT("Exact-account invalidation revokes the shared refresh flight"), Coordinator.Invalidate(Key));
	bool bAllTicketsStale = true;
	for (const FUnrealAICredentialRefreshTicket &Ticket : Tickets)
	{
		bAllTicketsStale &= !Coordinator.IsCurrent(Ticket);
	}
	TestTrue(TEXT("Invalidation makes every leader/follower ticket stale"), bAllTicketsStale);
	TestFalse(TEXT("Late provider completion cannot revive an invalidated account"),
				   Coordinator.Complete(Tickets[LeaderIndex]));

	FUnrealAICredentialRefreshTicket ReplacementLeader;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("Fresh demand after invalidation starts a replacement flight"),
				  Coordinator.BeginOrJoin(Key, ReplacementLeader, Error));
	TestTrue(TEXT("Replacement demand is a leader"), ReplacementLeader.IsLeader());
	TestTrue(TEXT("Replacement flight advances account generation"),
				  ReplacementLeader.GetAccountGeneration() > Tickets[LeaderIndex].GetAccountGeneration());
	TAtomic<int32> CompletionWinners{0};
	ParallelFor(64,
				[&Coordinator, &ReplacementLeader, &CompletionWinners](const int32)
				{
					if (Coordinator.Complete(ReplacementLeader))
					{
						++CompletionWinners;
					}
				});
	TestEqual(TEXT("Concurrent duplicate provider completion settles once"), CompletionWinners.Load(), 1);
	TestEqual(TEXT("Settled refresh flight leaves no active provider call"), Coordinator.NumActiveFlights(), 0);

	FUnrealAICredentialRefreshTicket ShutdownTicket;
	TestTrue(TEXT("Pre-shutdown refresh flight starts"), Coordinator.BeginOrJoin(Key, ShutdownTicket, Error));
	Coordinator.BeginShutdown();
	TestTrue(TEXT("Coordinator reports shutdown"), Coordinator.IsShutdown());
	TestFalse(TEXT("Shutdown invalidates an outstanding refresh ticket"), Coordinator.IsCurrent(ShutdownTicket));
	TestFalse(TEXT("Late provider completion cannot cross shutdown"), Coordinator.Complete(ShutdownTicket));
	FUnrealAICredentialRefreshTicket RejectedAfterShutdown;
	TestFalse(TEXT("Shutdown rejects new refresh demand"), Coordinator.BeginOrJoin(Key, RejectedAfterShutdown, Error));
	TestEqual(TEXT("Shutdown rejection uses a closed cancellation code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::CredentialCancelled);
	TestFalse(TEXT("Rejected shutdown demand returns no ticket"), RejectedAfterShutdown.IsValid());

	FUnrealAICredentialRefreshCoordinator CapacityCoordinator;
	for (int32 Index = 0; Index < FUnrealAICredentialRefreshCoordinator::MaxTrackedAccounts; ++Index)
	{
		FUnrealAICredentialRefreshTicket CapacityTicket;
		TestTrue(TEXT("Every refresh account through the exact capacity is admitted"),
					  CapacityCoordinator.BeginOrJoin(MakeRefreshKey(Index), CapacityTicket, Error));
		TestTrue(TEXT("Capacity fixture leader settles"), CapacityCoordinator.Complete(CapacityTicket));
	}
	TestEqual(TEXT("Refresh coordinator tracks its exact bounded account capacity"),
				   CapacityCoordinator.NumTrackedAccounts(), FUnrealAICredentialRefreshCoordinator::MaxTrackedAccounts);
	FUnrealAICredentialRefreshTicket OverCapacity;
	TestFalse(TEXT("Refresh coordinator rejects a new account beyond capacity"),
				   CapacityCoordinator.BeginOrJoin(
					   MakeRefreshKey(FUnrealAICredentialRefreshCoordinator::MaxTrackedAccounts), OverCapacity, Error));
	TestEqual(TEXT("Capacity rejection uses a closed busy code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity);

	FUnrealAICredentialRefreshTicket InvalidTicket;
	FUnrealAICredentialRefreshKey InvalidKey;
	TestFalse(TEXT("Invalid refresh identity fails before admission"),
				   CapacityCoordinator.BeginOrJoin(InvalidKey, InvalidTicket, Error));
	TestEqual(TEXT("Invalid refresh identity uses a closed request code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::InvalidRequest);
	return true;
}
