// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthAuthorizationKernel.h"
#include "CoreMinimal.h"

namespace UE::UnrealAI::Private
{
class FOAuthAuthorizationCoordinatorState;
}

/** Exact operation represented by one retained OAuth authorization completion. */
enum class EUnrealAIOAuthAuthorizationOperationKind : uint8
{
	Invalid,
	AuthorizeBrowserPkce,
	Revoke
};

/** Closed terminal vocabulary for the retained generic authorization coordinator. */
enum class EUnrealAIOAuthAuthorizationTerminalKind : uint8
{
	Invalid,
	Succeeded,
	Failed,
	Cancelled,
	TimedOut
};

/**
 * Move-only terminal result. AuthorizationResult is populated only for a successful browser-PKCE operation; revoke
 * success and every failure carry no credentials. Provider response text never crosses this boundary.
 */
struct UNREALAIAUTH_API FUnrealAIOAuthAuthorizationCompletion final
{
	FUnrealAIOAuthAuthorizationCompletion() = default;
	FUnrealAIOAuthAuthorizationCompletion(const FUnrealAIOAuthAuthorizationCompletion &) = delete;
	FUnrealAIOAuthAuthorizationCompletion &operator=(const FUnrealAIOAuthAuthorizationCompletion &) = delete;
	FUnrealAIOAuthAuthorizationCompletion(FUnrealAIOAuthAuthorizationCompletion &&) noexcept = default;
	FUnrealAIOAuthAuthorizationCompletion &operator=(FUnrealAIOAuthAuthorizationCompletion &&) noexcept = default;

	FUnrealAIRequestId RequestId;
	EUnrealAIOAuthAuthorizationOperationKind OperationKind = EUnrealAIOAuthAuthorizationOperationKind::Invalid;
	EUnrealAIOAuthAuthorizationTerminalKind TerminalKind = EUnrealAIOAuthAuthorizationTerminalKind::Invalid;
	FUnrealAIOAuthAuthorizationResult AuthorizationResult;
	FUnrealAIProviderAccessError Error;

	bool IsSuccess() const;
	bool ValidateShape(FString &OutError) const;
};

/** Thread-safe sink receiving exactly one terminal result for every admitted operation. */
class UNREALAIAUTH_API IUnrealAIOAuthAuthorizationCompletionSink
{
  public:
	virtual ~IUnrealAIOAuthAuthorizationCompletionSink() = default;
	virtual void CompleteOAuthAuthorization(FUnrealAIOAuthAuthorizationCompletion &&Completion) = 0;
};

/** Weak, prompt, nonblocking cancellation handle for one admitted operation. */
class UNREALAIAUTH_API IUnrealAIOAuthAuthorizationOperationHandle
{
  public:
	virtual ~IUnrealAIOAuthAuthorizationOperationHandle() = default;
	virtual FUnrealAIRequestId GetRequestId() const = 0;
	virtual void Cancel() = 0;
};

/** Bounded admission and deadline-monitor policy for generic authorization work. */
struct UNREALAIAUTH_API FUnrealAIOAuthAuthorizationCoordinatorConfig final
{
	static constexpr int32 MaxActiveOperationsLimit = 16;
	static constexpr double MinDeadlinePollSeconds = 0.001;
	static constexpr double MaxDeadlinePollSeconds = 1.0;

	int32 MaxActiveOperations = 4;
	double DeadlinePollSeconds = 0.01;
#if WITH_DEV_AUTOMATION_TESTS || WITH_PERF_AUTOMATION_TESTS
	/** Leaves deadline observation to PumpDeadlines for deterministic fake-clock tests. */
	bool bDisableBackgroundDeadlineMonitorForTesting = false;
#endif

	bool ValidateShape(FString &OutError) const;
};

/**
 * Retained asynchronous owner for generic browser-PKCE authorization and revocation.
 *
 * The coordinator admits a bounded number of operations, runs each synchronous executor call on a dedicated worker,
 * retains all inputs and sinks through physical settlement, and reserves exactly one logical terminal before invoking
 * user code. Cancellation, deadline, shutdown, and late executor completion therefore cannot double-deliver or revive
 * credentials. This class does not persist credentials; the later account transaction owns that boundary.
 */
class UNREALAIAUTH_API FUnrealAIOAuthAuthorizationCoordinator final
{
  public:
	FUnrealAIOAuthAuthorizationCoordinator(
		TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock,
		const FUnrealAIOAuthAuthorizationCoordinatorConfig &InConfig = {});
	~FUnrealAIOAuthAuthorizationCoordinator();
	FUnrealAIOAuthAuthorizationCoordinator(const FUnrealAIOAuthAuthorizationCoordinator &) = delete;
	FUnrealAIOAuthAuthorizationCoordinator &operator=(const FUnrealAIOAuthAuthorizationCoordinator &) = delete;

	bool
	StartAuthorizeBrowserPkce(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
							  TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> CompletionSink,
							  const FUnrealAICancellationToken &Cancellation,
							  TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> &OutHandle,
							  FUnrealAIProviderAccessError &OutError);
	bool StartRevoke(const FUnrealAIOAuthRevocationRequest &Request, FUnrealAISecretValue &&Token,
					 TSharedRef<IUnrealAIOAuthAuthorizationCompletionSink, ESPMode::ThreadSafe> CompletionSink,
					 const FUnrealAICancellationToken &Cancellation,
					 TSharedPtr<IUnrealAIOAuthAuthorizationOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					 FUnrealAIProviderAccessError &OutError);

	void BeginShutdown();
	bool IsShutdown() const;
	int32 GetActiveOperationCount() const;
	int32 GetPhysicalOperationCount() const;
	/** Deterministic executor seam; returns the number of operations newly settled by cancellation or deadline. */
	int32 PumpDeadlines();

  private:
	TSharedRef<UE::UnrealAI::Private::FOAuthAuthorizationCoordinatorState, ESPMode::ThreadSafe> State;
};
