// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthAuthorizationKernel.h"
#include "Auth/UnrealAIOAuthTokenEnvelope.h"
#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"
#include "Runtime/UnrealAICancellation.h"
#include "Runtime/UnrealAIClock.h"

/**
 * Trusted policy for one browser authorization whose credential becomes one atomic protected-store account record.
 *
 * Revocation is mandatory for this boundary. The exact issuer and public client used for authorization are reused for
 * every compensating revoke, and compensation owns a fresh bounded cancellation lineage after the caller cancels.
 */
struct UNREALAIAUTH_API FUnrealAIOAuthDurableAccountTransactionRequest final
{
	FUnrealAIOAuthBrowserAuthorizationRequest AuthorizationRequest;
	FUnrealAIOAuthTokenEnvelopeBinding Binding;
	FUnrealAISecretHandle SecretHandle;
	double CompensationTimeoutSeconds = FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds;
	bool bRequireRefreshToken = true;
	bool bRequireAccountRoutingValue = false;

	bool ValidateShape(FString &OutError) const;
};

/** Non-secret receipt proving that one authorization result won its protected-store CAS. */
struct UNREALAIAUTH_API FUnrealAIOAuthDurableAccountCommitResult final
{
	FUnrealAIRequestId RequestId;
	FUnrealAIOAuthTokenEnvelopeBinding Binding;
	uint64 SecretRevision = 0;
	FDateTime AccessTokenExpiresAtUtc;
	bool bRefreshCredentialPresent = false;
	FString SubjectFingerprint;
	TArray<FString> GrantedScopes;

	bool ValidateShape(FString &OutError) const;
	void Reset();
};

/**
 * Synchronous authorize-and-commit transaction used by retained account-provider workers.
 *
 * The transaction owns issued credentials until the secure-store CAS is durably settled. If encoding, loading, or
 * persistence fails, or if the supplied token is cancelled/times out after authorization (including sign-out fencing),
 * it removes only its exact attempted store value and revokes the newly issued grant through a fresh bounded issuer
 * operation, preferring its refresh credential when present. Callers must synchronously cancel this operation before
 * sign-out mutates the same account record.
 */
class UNREALAIAUTH_API FUnrealAIOAuthDurableAccountTransaction final
{
  public:
	static constexpr int32 MaxActiveAccountFences = 256;

	FUnrealAIOAuthDurableAccountTransaction(
		TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
		TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
		TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock);

	bool IsAvailable() const;
	/**
	 * Linearizes sign-out ahead of any unclaimed commit for this account. The sign-out owner must call this and cancel
	 * the operation token before loading/revoking/deleting the current record. A commit already claimed before this
	 * call is owned by that normal sign-out cleanup; an older unclaimed commit compensates itself.
	 */
	void InvalidateAccountForSignOut(const FUnrealAIOAuthTokenEnvelopeBinding &Binding);

	bool AuthorizeAndCommit(const FUnrealAIOAuthDurableAccountTransactionRequest &Request,
							const FUnrealAICancellationToken &Cancellation,
							FUnrealAIOAuthDurableAccountCommitResult &OutResult,
							FUnrealAIProviderAccessError &OutError);

  private:
	static bool TryCloneSecret(const FUnrealAISecretValue &Source, FUnrealAISecretValue &OutClone);
	static bool SecretsMatch(const FUnrealAISecretValue &A, const FUnrealAISecretValue &B);
	bool ReconcileAttemptedStore(const FUnrealAISecretHandle &Handle, uint64 ExpectedRevision, uint64 ReportedRevision,
								 const FUnrealAISecretValue &AttemptedValue) const;
	bool RevokeIssuedGrantToken(const FUnrealAIOAuthDurableAccountTransactionRequest &Request,
								FUnrealAISecretValue &&Token) const;
	uint64 BeginAuthorization(const FUnrealAIOAuthTokenEnvelopeBinding &Binding);
	void EndAuthorization(const FUnrealAIOAuthTokenEnvelopeBinding &Binding);
	bool IsCommitCandidateCurrent(const FUnrealAIOAuthTokenEnvelopeBinding &Binding, uint64 Generation,
								  const FUnrealAICancellationToken &Cancellation, const FUnrealAIDeadline &Deadline);

	struct FAccountFenceRecord final
	{
		FUnrealAIOAuthTokenEnvelopeBinding Binding;
		uint64 Generation = 0;
		int32 ActiveOperations = 0;
	};

	TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> Executor;
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> SecretStore;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	FCriticalSection FenceMutex;
	TArray<FAccountFenceRecord> AccountFences;
	bool bAvailable = false;
};
