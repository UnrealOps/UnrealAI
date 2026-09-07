// Copyright UnrealOps. All Rights Reserved.
#pragma once
#include "Auth/UnrealAIConnectionRegistry.h"
#include "Models/UnrealAIModelTypes.h"
#include "Runtime/UnrealAIClock.h"
#include "UnrealAIProviderReliability.generated.h"

/** Shared endpoint-health state. Breakers never grant provider or credential authority. */
UENUM(BlueprintType)
enum class EUnrealAIProviderCircuitState : uint8
{
	Closed,
	Open,
	HalfOpen
};

/** Bounded reliability policy frozen at run admission. */
USTRUCT(BlueprintType)
struct UNREALAIACCESS_API FUnrealAIProviderRetryPolicy
{
	GENERATED_BODY()

	static constexpr int32 MaxRetryAttemptsLimit = 32;
	static constexpr int32 MaxBreakerFailureThreshold = 1024;
	static constexpr int32 MaxHalfOpenProbesLimit = 64;
	static constexpr double MaxBackoffSecondsLimit = 3600.0;
	static constexpr double MaxBreakerOpenSecondsLimit = 24.0 * 60.0 * 60.0;

	/** Policy cap in addition to the run budget's MaxRetries. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability",
			  meta = (ClampMin = "0", ClampMax = "32"))
	int32 MaxRetryAttempts = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability",
			  meta = (ClampMin = "0.0", ClampMax = "3600.0"))
	double InitialBackoffSeconds = 0.25;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability",
			  meta = (ClampMin = "0.0", ClampMax = "3600.0"))
	double MaximumBackoffSeconds = 8.0;

	/** Symmetric deterministic jitter fraction in [0, 1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability",
			  meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double JitterFraction = 0.2;

	/** Provider hints above this value are clamped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability",
			  meta = (ClampMin = "0.0", ClampMax = "3600.0"))
	double MaximumRetryAfterSeconds = 60.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability",
			  meta = (ClampMin = "1", ClampMax = "1024"))
	int32 BreakerFailureThreshold = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability",
			  meta = (ClampMin = "0.001", ClampMax = "86400.0"))
	double BreakerOpenSeconds = 30.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UnrealAI|Reliability",
			  meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaximumHalfOpenProbes = 1;

	bool ValidateShape(FString &OutError) const;
};

/**
 * Exact access/provider/model-capability partition for one shared breaker.
 *
 * Keeping the complete frozen connection in the key prevents health observed
 * for one account, tenant, payer, billing mode, credential scheme, endpoint,
 * or destination policy from denying admission for another security domain.
 */
struct UNREALAIACCESS_API FUnrealAIProviderCircuitKey final
{
	FUnrealAIConnectionDescriptor Connection;
	FString ModelId;
	EUnrealAIModelCapability RequiredCapabilities = EUnrealAIModelCapability::None;

	static bool TryCreate(const FUnrealAIConnectionDescriptor &InConnection, const FString &InModelId,
						  EUnrealAIModelCapability InRequiredCapabilities, FUnrealAIProviderCircuitKey &OutKey,
						  FString &OutError);
	bool ValidateShape(FString &OutError) const;
	friend UNREALAIACCESS_API bool operator==(const FUnrealAIProviderCircuitKey &A,
											  const FUnrealAIProviderCircuitKey &B);
	friend UNREALAIACCESS_API uint32 GetTypeHash(const FUnrealAIProviderCircuitKey &Key);
};

namespace UE::UnrealAI::Private
{
class FUnrealAIProviderCircuitRegistryState;
}

/**
 * Move-only admission permit. Destruction neutrally releases an unreported
 * half-open probe so cancellation cannot strand the circuit.
 */
class UNREALAIACCESS_API FUnrealAIProviderCircuitPermit final
{
  public:
	FUnrealAIProviderCircuitPermit();
	~FUnrealAIProviderCircuitPermit();
	FUnrealAIProviderCircuitPermit(FUnrealAIProviderCircuitPermit &&Other) noexcept;
	FUnrealAIProviderCircuitPermit &operator=(FUnrealAIProviderCircuitPermit &&Other) noexcept;

	FUnrealAIProviderCircuitPermit(const FUnrealAIProviderCircuitPermit &) = delete;
	FUnrealAIProviderCircuitPermit &operator=(const FUnrealAIProviderCircuitPermit &) = delete;

	bool IsValid() const;
	bool ReportSuccess();
	bool ReportTransientFailure();
	bool ReportNonTransientResponse();
	bool Abandon();

  private:
	friend class FUnrealAIProviderCircuitRegistry;
	FUnrealAIProviderCircuitPermit(
		TSharedRef<UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState, ESPMode::ThreadSafe> InState,
		const FUnrealAIProviderCircuitKey &InKey, uint64 InPartitionIdentity, uint64 InGeneration,
		bool bInHalfOpenProbe);
	bool Complete(bool bSuccess, bool bCountsAsFailure);

	TSharedPtr<UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState, ESPMode::ThreadSafe> State;
	FUnrealAIProviderCircuitKey Key;
	uint64 PartitionIdentity = 0;
	uint64 Generation = 0;
	bool bHalfOpenProbe = false;
	bool bCompleted = false;
};

/** Thread-safe bounded breaker table with an injected monotonic clock. */
class UNREALAIACCESS_API FUnrealAIProviderCircuitRegistry final
{
  public:
	static constexpr int32 MaxCircuitEntriesLimit = 4096;

	FUnrealAIProviderCircuitRegistry(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
									 int32 InMaximumEntries = 1024);
	~FUnrealAIProviderCircuitRegistry();

	FUnrealAIProviderCircuitRegistry(const FUnrealAIProviderCircuitRegistry &) = delete;
	FUnrealAIProviderCircuitRegistry &operator=(const FUnrealAIProviderCircuitRegistry &) = delete;

	/**
	 * False means fail-fast with no provider or credential admission.
	 *
	 * On rejection, OutState preserves the closed vocabulary needed by callers:
	 * Closed denotes invalid input or bounded-registry exhaustion, Open denotes
	 * an open circuit, and HalfOpen denotes saturated probe capacity.
	 */
	bool TryBegin(const FUnrealAIProviderCircuitKey &Key, const FUnrealAIProviderRetryPolicy &Policy,
				  FUnrealAIProviderCircuitPermit &OutPermit, EUnrealAIProviderCircuitState &OutState,
				  FString &OutError);
	EUnrealAIProviderCircuitState GetState(const FUnrealAIProviderCircuitKey &Key,
										   const FUnrealAIProviderRetryPolicy &Policy) const;
	int32 Num() const;
	void Reset();

  private:
	TSharedRef<UE::UnrealAI::Private::FUnrealAIProviderCircuitRegistryState, ESPMode::ThreadSafe> State;
};

/** Shared deterministic retry math. Scheduling and charging belong to the caller. */
class UNREALAIACCESS_API FUnrealAIProviderRetryMath final
{
  public:
	static bool IsTransientCategory(EUnrealAIErrorCategory Category);
	static double ExponentialDelay(double Initial, double Maximum, int32 Ordinal, double Unit, double JitterFraction,
								   bool bSymmetric);
	static bool TryComputeDelay(const FUnrealAIProviderRetryPolicy &Policy, int32 Ordinal, uint64 Seed,
								double RetryAfterSeconds, double RemainingDeadlineSeconds, double &OutDelay);
};
