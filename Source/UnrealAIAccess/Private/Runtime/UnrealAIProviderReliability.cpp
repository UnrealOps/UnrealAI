// Copyright EngineWorks. All Rights Reserved.
#include "Runtime/UnrealAIProviderReliability.h"
#include "Math/NumericLimits.h"
#include "Misc/ScopeLock.h"

namespace
{
uint64 MixRetrySeed(uint64 Value)
{
	Value ^= Value >> 30;
	Value *= 0xbf58476d1ce4e5b9ULL;
	Value ^= Value >> 27;
	Value *= 0x94d049bb133111ebULL;
	Value ^= Value >> 31;
	return Value;
}

double DeterministicUnit(const uint64 Seed, const int32 RetryOrdinal)
{
	const uint64 Mixed = MixRetrySeed(Seed ^ (static_cast<uint64>(RetryOrdinal) * 0x9e3779b97f4a7c15ULL));
	return static_cast<double>(Mixed >> 11) * (1.0 / 9007199254740992.0);
}
} // namespace

bool FUnrealAIProviderRetryPolicy::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (MaxRetryAttempts < 0 || MaxRetryAttempts > MaxRetryAttemptsLimit || !FMath::IsFinite(InitialBackoffSeconds) ||
		InitialBackoffSeconds < 0.0 || InitialBackoffSeconds > MaxBackoffSecondsLimit ||
		!FMath::IsFinite(MaximumBackoffSeconds) || MaximumBackoffSeconds < InitialBackoffSeconds ||
		MaximumBackoffSeconds > MaxBackoffSecondsLimit || !FMath::IsFinite(JitterFraction) || JitterFraction < 0.0 ||
		JitterFraction > 1.0 || !FMath::IsFinite(MaximumRetryAfterSeconds) || MaximumRetryAfterSeconds < 0.0 ||
		MaximumRetryAfterSeconds > MaxBackoffSecondsLimit || BreakerFailureThreshold < 1 ||
		BreakerFailureThreshold > MaxBreakerFailureThreshold || !FMath::IsFinite(BreakerOpenSeconds) ||
		BreakerOpenSeconds <= 0.0 || BreakerOpenSeconds > MaxBreakerOpenSecondsLimit || MaximumHalfOpenProbes < 1 ||
		MaximumHalfOpenProbes > MaxHalfOpenProbesLimit)
	{
		OutError = TEXT("Provider retry policy contains an invalid retry, backoff, jitter, or circuit bound.");
		return false;
	}
	return true;
}

bool FUnrealAIProviderCircuitKey::TryCreate(const FUnrealAIConnectionDescriptor &InConnection, const FString &InModelId,
											const EUnrealAIModelCapability InRequiredCapabilities,
											FUnrealAIProviderCircuitKey &OutKey, FString &OutError)
{
	FUnrealAIProviderCircuitKey Candidate;
	Candidate.Connection = InConnection;
	Candidate.ModelId = InModelId;
	Candidate.RequiredCapabilities = InRequiredCapabilities;
	if (!Candidate.ValidateShape(OutError))
	{
		return false;
	}
	OutKey = MoveTemp(Candidate);
	return true;
}

bool FUnrealAIProviderCircuitKey::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString ConnectionError;
	if (!Connection.ValidateShape(ConnectionError) || ModelId.IsEmpty() || ModelId.TrimStartAndEnd() != ModelId ||
		FTCHARToUTF8(*ModelId).Length() > FUnrealAIModelRequest::MaxModelIdUtf8Bytes ||
		RequiredCapabilities == EUnrealAIModelCapability::None)
	{
		OutError = TEXT("Provider circuit key requires an exact frozen connection, bounded model, and capability set.");
		return false;
	}
	return true;
}

bool operator==(const FUnrealAIProviderCircuitKey &A, const FUnrealAIProviderCircuitKey &B)
{
	const FUnrealAICredentialDestination &AccessA = A.Connection.CredentialDestination;
	const FUnrealAICredentialDestination &AccessB = B.Connection.CredentialDestination;
	return A.Connection.SchemaVersion == B.Connection.SchemaVersion &&
		   A.Connection.ConnectionRevision == B.Connection.ConnectionRevision &&
		   A.Connection.ConnectionAlias == B.Connection.ConnectionAlias &&
		   A.Connection.EndpointProfileId == B.Connection.EndpointProfileId &&
		   AccessA.ModelProviderName == AccessB.ModelProviderName &&
		   AccessA.AccountAuthProviderName == AccessB.AccountAuthProviderName &&
		   AccessA.AuthProfileId == AccessB.AuthProfileId && AccessA.AccountId == AccessB.AccountId &&
		   AccessA.TenantRealm == AccessB.TenantRealm && AccessA.BillingPrincipalId == AccessB.BillingPrincipalId &&
		   AccessA.PayerHandle == AccessB.PayerHandle && AccessA.AuthScheme == AccessB.AuthScheme &&
		   AccessA.BillingMode == AccessB.BillingMode && AccessA.EndpointOrigin == AccessB.EndpointOrigin &&
		   AccessA.Audience == AccessB.Audience && AccessA.ConnectionRevision == AccessB.ConnectionRevision &&
		   AccessA.EndpointPolicyRevision == AccessB.EndpointPolicyRevision && A.ModelId == B.ModelId &&
		   A.RequiredCapabilities == B.RequiredCapabilities;
}

uint32 GetTypeHash(const FUnrealAIProviderCircuitKey &Key)
{
	const FUnrealAICredentialDestination &Access = Key.Connection.CredentialDestination;
	uint32 Hash = GetTypeHash(Key.Connection.SchemaVersion);
	Hash = HashCombine(Hash, GetTypeHash(Key.Connection.ConnectionRevision));
	Hash = HashCombine(Hash, GetTypeHash(Key.Connection.ConnectionAlias));
	Hash = HashCombine(Hash, GetTypeHash(Key.Connection.EndpointProfileId));
	Hash = HashCombine(Hash, GetTypeHash(Access.ModelProviderName));
	Hash = HashCombine(Hash, GetTypeHash(Access.AccountAuthProviderName));
	Hash = HashCombine(Hash, GetTypeHash(Access.AuthProfileId));
	Hash = HashCombine(Hash, GetTypeHash(Access.AccountId));
	Hash = HashCombine(Hash, GetTypeHash(Access.TenantRealm));
	Hash = HashCombine(Hash, GetTypeHash(Access.BillingPrincipalId));
	Hash = HashCombine(Hash, GetTypeHash(Access.PayerHandle));
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Access.AuthScheme)));
	Hash = HashCombine(Hash, GetTypeHash(static_cast<uint8>(Access.BillingMode)));
	Hash = HashCombine(Hash, GetTypeHash(Access.EndpointOrigin));
	Hash = HashCombine(Hash, GetTypeHash(Access.Audience));
	Hash = HashCombine(Hash, GetTypeHash(Access.ConnectionRevision));
	Hash = HashCombine(Hash, GetTypeHash(Access.EndpointPolicyRevision));
	Hash = HashCombine(Hash, GetTypeHash(Key.ModelId));
	return HashCombine(Hash, GetTypeHash(static_cast<uint32>(Key.RequiredCapabilities)));
}

namespace UE::UnrealAI::Private
{
class FUnrealAIProviderCircuitRegistryState final
	: public TSharedFromThis<FUnrealAIProviderCircuitRegistryState, ESPMode::ThreadSafe>
{
  public:
	struct FEntry final
	{
		uint64 PartitionIdentity = 0;
		EUnrealAIProviderCircuitState State = EUnrealAIProviderCircuitState::Closed;
		int32 ConsecutiveFailures = 0;
		int32 ActiveHalfOpenProbes = 0;
		double OpenUntil = 0.0;
		uint64 Generation = 1;
		int32 FailureThreshold = 1;
		int32 MaximumHalfOpenProbes = 1;
		double OpenDurationSeconds = 1.0;
	};

	FUnrealAIProviderCircuitRegistryState(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
										  const int32 InMaximumEntries)
		: Clock(MoveTemp(InClock)), MaximumEntries(InMaximumEntries)
	{
	}

	EUnrealAIProviderCircuitState RefreshLocked(FEntry &Entry) const
	{
		if (Entry.State == EUnrealAIProviderCircuitState::Open && Clock->MonotonicSeconds() >= Entry.OpenUntil)
		{
			Entry.State = EUnrealAIProviderCircuitState::HalfOpen;
			Entry.ActiveHalfOpenProbes = 0;
			++Entry.Generation;
		}
		return Entry.State;
	}

	bool Complete(const FUnrealAIProviderCircuitKey &Key, const uint64 PartitionIdentity, const uint64 Generation,
				  const bool bHalfOpenProbe, const bool bSuccess, const bool bCountsAsFailure)
	{
		FScopeLock Lock(&Mutex);
		TArray<FEntry> *Partitions = Entries.Find(Key);
		FEntry *Entry = Partitions == nullptr
							? nullptr
							: Partitions->FindByPredicate([PartitionIdentity](const FEntry &Candidate)
														  { return Candidate.PartitionIdentity == PartitionIdentity; });
		if (Entry == nullptr || Entry->Generation != Generation)
		{
			return false;
		}
		if (bHalfOpenProbe)
		{
			if (Entry->ActiveHalfOpenProbes <= 0)
			{
				return false;
			}
			--Entry->ActiveHalfOpenProbes;
		}
		if (!bSuccess && !bCountsAsFailure)
		{
			// Cancellation/owner loss is health-neutral. A half-open circuit
			// remains half-open and merely makes the probe slot available.
			return true;
		}
		if (bSuccess)
		{
			if (Entry->State == EUnrealAIProviderCircuitState::HalfOpen)
			{
				Entry->State = EUnrealAIProviderCircuitState::Closed;
				Entry->ConsecutiveFailures = 0;
				Entry->ActiveHalfOpenProbes = 0;
				Entry->OpenUntil = 0.0;
				++Entry->Generation;
			}
			else if (bSuccess)
			{
				Entry->ConsecutiveFailures = 0;
			}
			return true;
		}

		if (Entry->State == EUnrealAIProviderCircuitState::HalfOpen)
		{
			Entry->State = EUnrealAIProviderCircuitState::Open;
			Entry->ConsecutiveFailures = 0;
			Entry->ActiveHalfOpenProbes = 0;
			Entry->OpenUntil = Clock->MonotonicSeconds() + Entry->OpenDurationSeconds;
			++Entry->Generation;
			return true;
		}
		if (Entry->ConsecutiveFailures < TNumericLimits<int32>::Max())
		{
			++Entry->ConsecutiveFailures;
		}
		if (Entry->ConsecutiveFailures >= Entry->FailureThreshold)
		{
			Entry->State = EUnrealAIProviderCircuitState::Open;
			Entry->OpenUntil = Clock->MonotonicSeconds() + Entry->OpenDurationSeconds;
			++Entry->Generation;
		}
		return true;
	}

	mutable FCriticalSection Mutex;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TMap<FUnrealAIProviderCircuitKey, TArray<FEntry>> Entries;
	uint64 NextPartitionIdentity = 1;
	int32 EntryCount = 0;
	int32 MaximumEntries = 0;
};
} // namespace UE::UnrealAI::Private

// The policy values that opened a circuit must remain frozen in the entry so a
// completion does not consult mutable caller configuration.
namespace UE::UnrealAI::Private
{
static void FreezeCircuitPolicy(FUnrealAIProviderCircuitRegistryState::FEntry &Entry,
								const FUnrealAIProviderRetryPolicy &Policy)
{
	Entry.FailureThreshold = Policy.BreakerFailureThreshold;
	Entry.OpenDurationSeconds = Policy.BreakerOpenSeconds;
	Entry.MaximumHalfOpenProbes = Policy.MaximumHalfOpenProbes;
}

static bool CircuitPolicyMatches(const FUnrealAIProviderCircuitRegistryState::FEntry &Entry,
								 const FUnrealAIProviderRetryPolicy &Policy)
{
	return Entry.FailureThreshold == Policy.BreakerFailureThreshold &&
		   Entry.OpenDurationSeconds == Policy.BreakerOpenSeconds &&
		   Entry.MaximumHalfOpenProbes == Policy.MaximumHalfOpenProbes;
}
} // namespace UE::UnrealAI::Private

FUnrealAIProviderCircuitPermit::FUnrealAIProviderCircuitPermit() = default;

FUnrealAIProviderCircuitPermit::FUnrealAIProviderCircuitPermit(
	TSharedRef<UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState, ESPMode::ThreadSafe> InState,
	const FUnrealAIProviderCircuitKey &InKey, const uint64 InPartitionIdentity, const uint64 InGeneration,
	const bool bInHalfOpenProbe)
	: State(MoveTemp(InState)), Key(InKey), PartitionIdentity(InPartitionIdentity), Generation(InGeneration),
	  bHalfOpenProbe(bInHalfOpenProbe)
{
}

FUnrealAIProviderCircuitPermit::~FUnrealAIProviderCircuitPermit()
{
	Abandon();
}

FUnrealAIProviderCircuitPermit::FUnrealAIProviderCircuitPermit(FUnrealAIProviderCircuitPermit &&Other) noexcept
	: State(MoveTemp(Other.State)), Key(MoveTemp(Other.Key)), PartitionIdentity(Other.PartitionIdentity),
	  Generation(Other.Generation), bHalfOpenProbe(Other.bHalfOpenProbe), bCompleted(Other.bCompleted)
{
	Other.bCompleted = true;
}

FUnrealAIProviderCircuitPermit &
FUnrealAIProviderCircuitPermit::operator=(FUnrealAIProviderCircuitPermit &&Other) noexcept
{
	if (this != &Other)
	{
		Abandon();
		State = MoveTemp(Other.State);
		Key = MoveTemp(Other.Key);
		PartitionIdentity = Other.PartitionIdentity;
		Generation = Other.Generation;
		bHalfOpenProbe = Other.bHalfOpenProbe;
		bCompleted = Other.bCompleted;
		Other.bCompleted = true;
	}
	return *this;
}

bool FUnrealAIProviderCircuitPermit::IsValid() const
{
	return State.IsValid() && !bCompleted && PartitionIdentity != 0 && Generation != 0;
}

bool FUnrealAIProviderCircuitPermit::Complete(const bool bSuccess, const bool bCountsAsFailure)
{
	if (!IsValid())
	{
		return false;
	}
	bCompleted = true;
	const TSharedPtr<UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState, ESPMode::ThreadSafe> LocalState =
		MoveTemp(State);
	return LocalState->Complete(Key, PartitionIdentity, Generation, bHalfOpenProbe, bSuccess, bCountsAsFailure);
}

bool FUnrealAIProviderCircuitPermit::ReportSuccess()
{
	return Complete(true, false);
}

bool FUnrealAIProviderCircuitPermit::ReportTransientFailure()
{
	return Complete(false, true);
}

bool FUnrealAIProviderCircuitPermit::ReportNonTransientResponse()
{
	return Complete(true, false);
}

bool FUnrealAIProviderCircuitPermit::Abandon()
{
	return Complete(false, false);
}

FUnrealAIProviderCircuitRegistry::FUnrealAIProviderCircuitRegistry(
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock, const int32 InMaximumEntries)
	: State(MakeShared<UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState, ESPMode::ThreadSafe>(
		  MoveTemp(InClock), FMath::Clamp(InMaximumEntries, 1, MaxCircuitEntriesLimit)))
{
}

FUnrealAIProviderCircuitRegistry::~FUnrealAIProviderCircuitRegistry() = default;

bool FUnrealAIProviderCircuitRegistry::TryBegin(const FUnrealAIProviderCircuitKey &Key,
												const FUnrealAIProviderRetryPolicy &Policy,
												FUnrealAIProviderCircuitPermit &OutPermit,
												EUnrealAIProviderCircuitState &OutState, FString &OutError)
{
	OutError.Reset();
	// Closed is the non-transient rejection sentinel. Open and HalfOpen are
	// assigned only after an exact, valid registry partition is found.
	OutState = EUnrealAIProviderCircuitState::Closed;
	FString ShapeError;
	if (OutPermit.IsValid() || !Key.ValidateShape(ShapeError) || !Policy.ValidateShape(ShapeError))
	{
		OutError = TEXT("Provider circuit admission requires valid policy/key input and an empty permit.");
		return false;
	}
	FScopeLock Lock(&State->Mutex);
	TArray<UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState::FEntry> *Partitions = State->Entries.Find(Key);
	UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState::FEntry *Entry =
		Partitions == nullptr
			? nullptr
			: Partitions->FindByPredicate(
				  [&Policy](const UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState::FEntry &Candidate)
				  { return UE::UnrealAI::Private::CircuitPolicyMatches(Candidate, Policy); });
	if (Entry == nullptr)
	{
		if (State->EntryCount >= State->MaximumEntries || State->NextPartitionIdentity == 0)
		{
			OutError = TEXT("Provider circuit registry reached its bounded capacity.");
			return false;
		}
		if (Partitions == nullptr)
		{
			Partitions = &State->Entries.Add(Key);
		}
		UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState::FEntry NewEntry;
		NewEntry.PartitionIdentity = State->NextPartitionIdentity;
		State->NextPartitionIdentity =
			State->NextPartitionIdentity == MAX_uint64 ? 0 : State->NextPartitionIdentity + 1;
		Entry = &Partitions->Add_GetRef(MoveTemp(NewEntry));
		UE::UnrealAI::Private::FreezeCircuitPolicy(*Entry, Policy);
		++State->EntryCount;
	}
	OutState = State->RefreshLocked(*Entry);
	if (OutState == EUnrealAIProviderCircuitState::Open)
	{
		OutError = TEXT("Provider circuit is open.");
		return false;
	}
	const bool bHalfOpen = OutState == EUnrealAIProviderCircuitState::HalfOpen;
	if (bHalfOpen && Entry->ActiveHalfOpenProbes >= Entry->MaximumHalfOpenProbes)
	{
		OutError = TEXT("Provider circuit has no available half-open probe.");
		return false;
	}
	if (bHalfOpen)
	{
		++Entry->ActiveHalfOpenProbes;
	}
	OutPermit = FUnrealAIProviderCircuitPermit(State, Key, Entry->PartitionIdentity, Entry->Generation, bHalfOpen);
	return true;
}

EUnrealAIProviderCircuitState
FUnrealAIProviderCircuitRegistry::GetState(const FUnrealAIProviderCircuitKey &Key,
										   const FUnrealAIProviderRetryPolicy &Policy) const
{
	FScopeLock Lock(&State->Mutex);
	FString ShapeError;
	if (!Key.ValidateShape(ShapeError) || !Policy.ValidateShape(ShapeError))
	{
		return EUnrealAIProviderCircuitState::Open;
	}
	TArray<UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState::FEntry> *Partitions = State->Entries.Find(Key);
	UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState::FEntry *Entry =
		Partitions == nullptr
			? nullptr
			: Partitions->FindByPredicate(
				  [&Policy](const UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState::FEntry &Candidate)
				  { return UE::UnrealAI::Private::CircuitPolicyMatches(Candidate, Policy); });
	if (Entry == nullptr)
	{
		return EUnrealAIProviderCircuitState::Closed;
	}
	return State->RefreshLocked(*Entry);
}

int32 FUnrealAIProviderCircuitRegistry::Num() const
{
	FScopeLock Lock(&State->Mutex);
	return State->EntryCount;
}

void FUnrealAIProviderCircuitRegistry::Reset()
{
	FScopeLock Lock(&State->Mutex);
	State->Entries.Reset();
	State->EntryCount = 0;
}

bool FUnrealAIProviderRetryMath::IsTransientCategory(EUnrealAIErrorCategory Category)
{
	return Category == EUnrealAIErrorCategory::Busy || Category == EUnrealAIErrorCategory::RateLimited ||
		   Category == EUnrealAIErrorCategory::Timeout || Category == EUnrealAIErrorCategory::Transport ||
		   Category == EUnrealAIErrorCategory::Provider;
}

double FUnrealAIProviderRetryMath::ExponentialDelay(double Initial, double Maximum, int32 Ordinal, double Unit,
													double JitterFraction, bool bSymmetric)
{
	if (!FMath::IsFinite(Initial) || !FMath::IsFinite(Maximum) || Initial < 0.0 || Maximum < Initial || Ordinal < 1 ||
		!FMath::IsFinite(Unit) || !FMath::IsFinite(JitterFraction))
	{
		return Maximum;
	}
	const int32 Exponent = FMath::Min(Ordinal - 1, 62);
	double Base = FMath::Min(Maximum, Initial * FMath::Pow(2.0, static_cast<double>(Exponent)));
	if (!FMath::IsFinite(Base))
	{
		Base = Maximum;
	}
	const double Sample = FMath::Clamp(Unit, 0.0, 1.0);
	const double Multiplier = 1.0 + FMath::Clamp(JitterFraction, 0.0, 1.0) * (bSymmetric ? 2.0 * Sample - 1.0 : Sample);
	return FMath::Clamp(Base * Multiplier, 0.0, Maximum);
}

bool FUnrealAIProviderRetryMath::TryComputeDelay(const FUnrealAIProviderRetryPolicy &Policy, int32 Ordinal, uint64 Seed,
												 double RetryAfterSeconds, double RemainingDeadlineSeconds,
												 double &OutDelay)
{
	OutDelay = 0.0;
	FString Error;
	if (!Policy.ValidateShape(Error) || Ordinal < 1 || Ordinal > Policy.MaxRetryAttempts ||
		!FMath::IsFinite(RetryAfterSeconds) || !FMath::IsFinite(RemainingDeadlineSeconds) ||
		RemainingDeadlineSeconds <= 0.0)
	{
		return false;
	}
	const double Jittered = ExponentialDelay(Policy.InitialBackoffSeconds, Policy.MaximumBackoffSeconds, Ordinal,
											 DeterministicUnit(Seed, Ordinal), Policy.JitterFraction, true);
	const double ProviderDelay = FMath::Clamp(RetryAfterSeconds, 0.0, Policy.MaximumRetryAfterSeconds);
	const double Delay = FMath::Max(Jittered, ProviderDelay);
	if (!FMath::IsFinite(Delay) || Delay >= RemainingDeadlineSeconds)
	{
		return false;
	}
	OutDelay = Delay;
	return true;
}
