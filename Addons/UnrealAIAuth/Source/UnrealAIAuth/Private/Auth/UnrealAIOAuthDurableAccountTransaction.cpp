// Copyright EngineWorks. All Rights Reserved.

#include "Auth/UnrealAIOAuthDurableAccountTransaction.h"

#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace
{
FUnrealAIProviderAccessError MakeDurableAccountError(const EUnrealAIErrorCategory Category,
													 const EUnrealAIProviderAccessErrorCode Code,
													 const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

FUnrealAIProviderAccessError NormalizeDurableAccountAuthorizationError(const FUnrealAIProviderAccessError &Candidate)
{
	FString ShapeError;
	if (!Candidate.IsError() || !Candidate.ValidateShape(ShapeError))
	{
		return MakeDurableAccountError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
	}
	return Candidate;
}

FUnrealAIProviderAccessError MakeDurableAccountObservedTerminalError(const FUnrealAICancellationToken &Cancellation,
																	 const FUnrealAIDeadline &Deadline,
																	 const IUnrealAIClock &Clock)
{
	if (Cancellation.IsCancellationRequested())
	{
		if (Cancellation.GetReason() == EUnrealAICancellationReason::Timeout)
		{
			return MakeDurableAccountError(EUnrealAIErrorCategory::Timeout,
										   EUnrealAIProviderAccessErrorCode::AuthTimedOut, true);
		}
		return MakeDurableAccountError(EUnrealAIErrorCategory::Cancelled,
									   EUnrealAIProviderAccessErrorCode::AuthCancelled);
	}
	if (Deadline.IsExpired(Clock))
	{
		return MakeDurableAccountError(EUnrealAIErrorCategory::Timeout, EUnrealAIProviderAccessErrorCode::AuthTimedOut,
									   true);
	}
	return {};
}

bool IsDurableAccountBase64UrlFingerprint(const FString &Value)
{
	if (Value.Len() != 43)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!((Character >= TEXT('A') && Character <= TEXT('Z')) ||
			   (Character >= TEXT('a') && Character <= TEXT('z')) ||
				(Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('-') || Character == TEXT('_')))
		{
			return false;
		}
	}
	return true;
}

bool IsDurableAccountScope(const FString &Scope)
{
	FTCHARToUTF8 Utf8(*Scope);
	if (Scope.IsEmpty() || Utf8.Length() <= 0 || Utf8.Length() > 256)
	{
		return false;
	}
	for (const TCHAR Character : Scope)
	{
		const bool bAllowed =
			Character == 0x21 || (Character >= 0x23 && Character <= 0x5b) || (Character >= 0x5d && Character <= 0x7e);
		if (!bAllowed)
		{
			return false;
		}
	}
	return true;
}

bool ValidateDurableAccountGrantedScopes(const TArray<FString> &Scopes)
{
	if (Scopes.IsEmpty() || Scopes.Num() > FUnrealAIOAuthBrowserAuthorizationRequest::MaxScopes)
	{
		return false;
	}
	TSet<FString> UniqueScopes;
	for (const FString &Scope : Scopes)
	{
		if (!IsDurableAccountScope(Scope) || UniqueScopes.Contains(Scope))
		{
			return false;
		}
		UniqueScopes.Add(Scope);
	}
	return true;
}

bool IsDurableAccountAuthorizationMetadataValid(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
												const FUnrealAIOAuthAuthorizationResult &Result)
{
	if (!IsDurableAccountBase64UrlFingerprint(Result.SubjectFingerprint) ||
		!ValidateDurableAccountGrantedScopes(Result.GrantedScopes))
	{
		return false;
	}
	for (const FString &RequestedScope : Request.RequestedScopes)
	{
		if (!Result.GrantedScopes.Contains(RequestedScope))
		{
			return false;
		}
	}
	return true;
}

bool IsDurableAccountStoreSuccessShape(const EUnrealAISecretStoreResult Result, const FUnrealAISecretValue &Value,
									   const uint64 Revision)
{
	return (Result == EUnrealAISecretStoreResult::Succeeded && Value.IsSet() && Revision != 0) ||
		   (Result == EUnrealAISecretStoreResult::NotFound && !Value.IsSet() && Revision == 0);
}
} // namespace

bool FUnrealAIOAuthDurableAccountTransactionRequest::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString NestedError;
	if (!AuthorizationRequest.ValidateShape(NestedError))
	{
		OutError = TEXT("Authorization request is invalid.");
		return false;
	}
	if (AuthorizationRequest.Server.RevocationEndpoint.IsEmpty())
	{
		OutError = TEXT("A trusted revocation endpoint is required for compensating revocation.");
		return false;
	}
	if (!Binding.ValidateShape(NestedError))
	{
		OutError = TEXT("OAuth token binding is invalid.");
		return false;
	}
	if (!SecretHandle.ValidateShape(NestedError))
	{
		OutError = TEXT("Secret handle is invalid.");
		return false;
	}
	if (!FMath::IsFinite(CompensationTimeoutSeconds) || CompensationTimeoutSeconds <= 0.0 ||
		CompensationTimeoutSeconds > FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds)
	{
		OutError = TEXT("Compensation timeout is outside the supported range.");
		return false;
	}
	return true;
}

bool FUnrealAIOAuthDurableAccountCommitResult::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString NestedError;
	if (!RequestId.IsValid() || !Binding.ValidateShape(NestedError) || SecretRevision == 0 ||
		AccessTokenExpiresAtUtc.GetTicks() <= 0 || !IsDurableAccountBase64UrlFingerprint(SubjectFingerprint) ||
		!ValidateDurableAccountGrantedScopes(GrantedScopes))
	{
		OutError = TEXT("Durable OAuth account commit result is invalid.");
		return false;
	}
	return true;
}

void FUnrealAIOAuthDurableAccountCommitResult::Reset()
{
	RequestId = {};
	Binding = {};
	SecretRevision = 0;
	AccessTokenExpiresAtUtc = {};
	bRefreshCredentialPresent = false;
	SubjectFingerprint.Reset();
	GrantedScopes.Reset();
}

FUnrealAIOAuthDurableAccountTransaction::FUnrealAIOAuthDurableAccountTransaction(
	TSharedRef<IUnrealAIOAuthAuthorizationExecutor, ESPMode::ThreadSafe> InExecutor,
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InSecretStore,
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock)
	: Executor(MoveTemp(InExecutor)), SecretStore(MoveTemp(InSecretStore)), Clock(MoveTemp(InClock))
{
	const FUnrealAISecretStoreCapabilities Capabilities = SecretStore->DescribeCapabilities();
	FString CapabilityError;
	bAvailable = Capabilities.ValidateShape(CapabilityError) && Capabilities.bAvailableInCurrentBuild &&
				 Capabilities.PersistenceClass == EUnrealAISecretStorePersistenceClass::Persistent &&
				 Capabilities.IsProductionProtected() && Capabilities.bAtomicCompareAndSwap;
#if UE_BUILD_SHIPPING
	bAvailable = bAvailable && Capabilities.bAvailableInShipping;
#endif
#if UE_BUILD_SHIPPING && UE_SERVER
	bAvailable = bAvailable &&
				 Capabilities.ProtectionClass == EUnrealAISecretStoreProtectionClass::ExternalSecretService &&
				 Capabilities.ScopeClass == EUnrealAISecretStoreScopeClass::Service;
#endif
}

bool FUnrealAIOAuthDurableAccountTransaction::IsAvailable() const
{
	return bAvailable;
}

void FUnrealAIOAuthDurableAccountTransaction::InvalidateAccountForSignOut(
	const FUnrealAIOAuthTokenEnvelopeBinding &Binding)
{
	FString ShapeError;
	if (!Binding.ValidateShape(ShapeError))
	{
		return;
	}
	FScopeLock Lock(&FenceMutex);
	for (FAccountFenceRecord &Record : AccountFences)
	{
		if (Record.Binding == Binding)
		{
			++Record.Generation;
			if (Record.Generation == 0)
			{
				Record.Generation = 1;
			}
			return;
		}
	}
}

bool FUnrealAIOAuthDurableAccountTransaction::AuthorizeAndCommit(
	const FUnrealAIOAuthDurableAccountTransactionRequest &Request, const FUnrealAICancellationToken &Cancellation,
	FUnrealAIOAuthDurableAccountCommitResult &OutResult, FUnrealAIProviderAccessError &OutError)
{
	OutResult.Reset();
	OutError = {};

	FString ShapeError;
	if (!Request.ValidateShape(ShapeError))
	{
		OutError = MakeDurableAccountError(EUnrealAIErrorCategory::InvalidArgument,
										   EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	if (!Cancellation.IsValid())
	{
		OutError = MakeDurableAccountError(EUnrealAIErrorCategory::InvalidArgument,
										   EUnrealAIProviderAccessErrorCode::InvalidRequest);
		return false;
	}
	if (!bAvailable)
	{
		OutError = MakeDurableAccountError(EUnrealAIErrorCategory::UnsupportedCapability,
										   EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
		return false;
	}
	if (Request.SecretHandle.StoreName != SecretStore->GetStoreName())
	{
		OutError = MakeDurableAccountError(EUnrealAIErrorCategory::InvalidArgument,
										   EUnrealAIProviderAccessErrorCode::SecretHandleStoreMismatch);
		return false;
	}
	const uint64 AccountGeneration = BeginAuthorization(Request.Binding);
	if (AccountGeneration == 0)
	{
		OutError = MakeDurableAccountError(EUnrealAIErrorCategory::Busy,
										   EUnrealAIProviderAccessErrorCode::OperationBusy, true);
		return false;
	}
	ON_SCOPE_EXIT
	{
		EndAuthorization(Request.Binding);
	};

	const FUnrealAIDeadline Deadline = FUnrealAIDeadline::FromNow(*Clock, Request.AuthorizationRequest.TimeoutSeconds);
	OutError = MakeDurableAccountObservedTerminalError(Cancellation, Deadline, *Clock);
	if (OutError.IsError())
	{
		return false;
	}

	FUnrealAIOAuthAuthorizationResult AuthorizationResult;
	FUnrealAIProviderAccessError AuthorizationError;
	if (!Executor->AuthorizeBrowserPkce(Request.AuthorizationRequest, Cancellation, AuthorizationResult,
										AuthorizationError))
	{
		FUnrealAISecretValue *IssuedGrantCredential = &AuthorizationResult.Tokens.AccessToken;
		if (AuthorizationResult.Tokens.RefreshToken.IsSet())
		{
			IssuedGrantCredential = &AuthorizationResult.Tokens.RefreshToken;
		}
		const bool bHadIssuedGrantCredential = IssuedGrantCredential->IsSet();
		bool bRevokedUnexpectedToken = true;
		if (bHadIssuedGrantCredential)
		{
			bRevokedUnexpectedToken = RevokeIssuedGrantToken(Request, MoveTemp(*IssuedGrantCredential));
		}
		AuthorizationResult.Reset();
		if (!bRevokedUnexpectedToken)
		{
			OutError = MakeDurableAccountError(EUnrealAIErrorCategory::Persistence,
											   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true);
			return false;
		}
		OutError = MakeDurableAccountObservedTerminalError(Cancellation, Deadline, *Clock);
		if (!OutError.IsError())
		{
			OutError = NormalizeDurableAccountAuthorizationError(AuthorizationError);
		}
		return false;
	}
	ON_SCOPE_EXIT
	{
		AuthorizationResult.Reset();
	};

	FUnrealAISecretValue RevocationToken;
	FUnrealAISecretValue *IssuedGrantCredential = &AuthorizationResult.Tokens.AccessToken;
	if (AuthorizationResult.Tokens.RefreshToken.IsSet())
	{
		IssuedGrantCredential = &AuthorizationResult.Tokens.RefreshToken;
	}
	if (!IssuedGrantCredential->IsSet())
	{
		OutError = MakeDurableAccountError(EUnrealAIErrorCategory::Provider,
										   EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
		return false;
	}
	if (!TryCloneSecret(*IssuedGrantCredential, RevocationToken))
	{
		RevocationToken = MoveTemp(*IssuedGrantCredential);
		const bool bRevoked = RevokeIssuedGrantToken(Request, MoveTemp(RevocationToken));
		OutError =
			MakeDurableAccountError(bRevoked ? EUnrealAIErrorCategory::Internal : EUnrealAIErrorCategory::Persistence,
									bRevoked ? EUnrealAIProviderAccessErrorCode::SecretCopyFailed
											 : EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed,
									!bRevoked);
		return false;
	}
	ON_SCOPE_EXIT
	{
		RevocationToken.Reset();
	};

	auto FailAfterAuthorization = [&](const FUnrealAIProviderAccessError &PrimaryError, const bool bLocalResolved)
	{
		const bool bRevoked = RevokeIssuedGrantToken(Request, MoveTemp(RevocationToken));
		if (!bLocalResolved || !bRevoked)
		{
			OutError = MakeDurableAccountError(EUnrealAIErrorCategory::Persistence,
											   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true);
		}
		else
		{
			OutError = PrimaryError;
		}
		return false;
	};

	FUnrealAIProviderAccessError TerminalError =
		MakeDurableAccountObservedTerminalError(Cancellation, Deadline, *Clock);
	if (TerminalError.IsError() ||
		!IsCommitCandidateCurrent(Request.Binding, AccountGeneration, Cancellation, Deadline))
	{
		if (!TerminalError.IsError())
		{
			TerminalError = MakeDurableAccountError(EUnrealAIErrorCategory::Cancelled,
													EUnrealAIProviderAccessErrorCode::AuthCancelled);
		}
		return FailAfterAuthorization(TerminalError, true);
	}

	const FDateTime AccessExpiry = AuthorizationResult.Tokens.AccessTokenExpiresAtUtc;
	const bool bRefreshPresent = AuthorizationResult.Tokens.RefreshToken.IsSet();
	if (!AuthorizationResult.Tokens.AccessToken.IsSet())
	{
		return FailAfterAuthorization(MakeDurableAccountError(EUnrealAIErrorCategory::Provider,
															  EUnrealAIProviderAccessErrorCode::AuthResponseInvalid),
									  true);
	}
	if (!IsDurableAccountAuthorizationMetadataValid(Request.AuthorizationRequest, AuthorizationResult))
	{
		return FailAfterAuthorization(MakeDurableAccountError(EUnrealAIErrorCategory::Provider,
															  EUnrealAIProviderAccessErrorCode::AuthResponseInvalid),
									  true);
	}
	if ((Request.bRequireRefreshToken && !bRefreshPresent) ||
		(Request.bRequireAccountRoutingValue && !AuthorizationResult.Tokens.AccountRoutingValue.IsSet()))
	{
		return FailAfterAuthorization(
			MakeDurableAccountError(EUnrealAIErrorCategory::Provider,
									EUnrealAIProviderAccessErrorCode::AuthCredentialIncomplete),
			true);
	}

	FUnrealAIOAuthTokenEnvelope Envelope;
	FString EnvelopeError;
	const FDateTime EncodeNow = Clock->UtcNow();
	if (!FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(Request.Binding, MoveTemp(AuthorizationResult.Tokens), EncodeNow,
													 Envelope, EnvelopeError))
	{
		const EUnrealAIProviderAccessErrorCode Code =
			AccessExpiry.GetTicks() % ETimespan::TicksPerSecond != 0 || AccessExpiry <= Clock->UtcNow()
				? EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid
				: EUnrealAIProviderAccessErrorCode::AuthResponseInvalid;
		return FailAfterAuthorization(MakeDurableAccountError(EUnrealAIErrorCategory::Provider, Code), true);
	}

	FUnrealAISecretValue Encoded;
	if (!FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(Envelope), EncodeNow, Encoded, EnvelopeError))
	{
		return FailAfterAuthorization(MakeDurableAccountError(EUnrealAIErrorCategory::Provider,
															  EUnrealAIProviderAccessErrorCode::AuthResponseInvalid),
									  true);
	}
	ON_SCOPE_EXIT
	{
		Encoded.Reset();
	};

	TerminalError = MakeDurableAccountObservedTerminalError(Cancellation, Deadline, *Clock);
	if (TerminalError.IsError() ||
		!IsCommitCandidateCurrent(Request.Binding, AccountGeneration, Cancellation, Deadline))
	{
		if (!TerminalError.IsError())
		{
			TerminalError = MakeDurableAccountError(EUnrealAIErrorCategory::Cancelled,
													EUnrealAIProviderAccessErrorCode::AuthCancelled);
		}
		return FailAfterAuthorization(TerminalError, true);
	}

	FUnrealAISecretStoreOperationContext StoreContext;
	const double StoreTimeout =
		FMath::Min(Deadline.RemainingSeconds(*Clock), FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds);
	if (StoreTimeout <= 0.0 ||
		!FUnrealAISecretStoreOperationContext::TryCreate(Clock, StoreTimeout, Cancellation, StoreContext, ShapeError))
	{
		TerminalError = MakeDurableAccountObservedTerminalError(Cancellation, Deadline, *Clock);
		if (!TerminalError.IsError())
		{
			TerminalError = MakeDurableAccountError(EUnrealAIErrorCategory::Persistence,
													EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed, true);
		}
		return FailAfterAuthorization(TerminalError, true);
	}

	FUnrealAISecretValue Existing;
	uint64 ExistingRevision = 0;
	FUnrealAIProviderAccessError StoreError;
	const EUnrealAISecretStoreResult LoadResult =
		SecretStore->Load(StoreContext, Request.SecretHandle, Existing, ExistingRevision, StoreError);
	bool bLoadShapeValid = IsDurableAccountStoreSuccessShape(LoadResult, Existing, ExistingRevision);
	if (bLoadShapeValid && LoadResult == EUnrealAISecretStoreResult::Succeeded)
	{
		FUnrealAIOAuthTokenEnvelope ExistingEnvelope;
		FString ExistingEnvelopeError;
		bLoadShapeValid = FUnrealAIOAuthTokenEnvelopeCodec::TryDecodeForRefresh(
			MoveTemp(Existing), Request.Binding, Clock->UtcNow(), ExistingEnvelope, ExistingEnvelopeError);
		ExistingEnvelope.Reset();
	}
	else
	{
		Existing.Reset();
	}
	TerminalError = MakeDurableAccountObservedTerminalError(Cancellation, Deadline, *Clock);
	if (TerminalError.IsError() ||
		!IsCommitCandidateCurrent(Request.Binding, AccountGeneration, Cancellation, Deadline))
	{
		if (!TerminalError.IsError())
		{
			TerminalError = MakeDurableAccountError(EUnrealAIErrorCategory::Cancelled,
													EUnrealAIProviderAccessErrorCode::AuthCancelled);
		}
		return FailAfterAuthorization(TerminalError, true);
	}
	if (!bLoadShapeValid)
	{
		return FailAfterAuthorization(
			MakeDurableAccountError(EUnrealAIErrorCategory::Persistence,
									EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed,
									StoreError.bRetryable || LoadResult == EUnrealAISecretStoreResult::TimedOut ||
										LoadResult == EUnrealAISecretStoreResult::Unavailable ||
										LoadResult == EUnrealAISecretStoreResult::Locked),
			true);
	}

	const uint64 ExpectedRevision = LoadResult == EUnrealAISecretStoreResult::Succeeded ? ExistingRevision : 0;
	uint64 NewRevision = 0;
	const EUnrealAISecretStoreResult StoreResult =
		SecretStore->Store(StoreContext, Request.SecretHandle, Encoded, ExpectedRevision, NewRevision, StoreError);
	if (StoreResult != EUnrealAISecretStoreResult::Succeeded || NewRevision == 0)
	{
		const bool bLocalResolved =
			ReconcileAttemptedStore(Request.SecretHandle, ExpectedRevision, NewRevision, Encoded);
		return FailAfterAuthorization(
			MakeDurableAccountError(EUnrealAIErrorCategory::Persistence,
									EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed,
									StoreError.bRetryable || StoreResult == EUnrealAISecretStoreResult::TimedOut ||
										StoreResult == EUnrealAISecretStoreResult::Unavailable ||
										StoreResult == EUnrealAISecretStoreResult::Locked),
			bLocalResolved);
	}

	TerminalError = MakeDurableAccountObservedTerminalError(Cancellation, Deadline, *Clock);
	if (TerminalError.IsError())
	{
		const bool bLocalResolved =
			ReconcileAttemptedStore(Request.SecretHandle, ExpectedRevision, NewRevision, Encoded);
		return FailAfterAuthorization(TerminalError, bLocalResolved);
	}
	if (AccessExpiry <= Clock->UtcNow())
	{
		const bool bLocalResolved =
			ReconcileAttemptedStore(Request.SecretHandle, ExpectedRevision, NewRevision, Encoded);
		return FailAfterAuthorization(MakeDurableAccountError(EUnrealAIErrorCategory::Provider,
															  EUnrealAIProviderAccessErrorCode::AuthExpiryInvalid),
									  bLocalResolved);
	}

	OutResult.RequestId = Request.AuthorizationRequest.RequestId;
	OutResult.Binding = Request.Binding;
	OutResult.SecretRevision = NewRevision;
	OutResult.AccessTokenExpiresAtUtc = AccessExpiry;
	OutResult.bRefreshCredentialPresent = bRefreshPresent;
	OutResult.SubjectFingerprint = MoveTemp(AuthorizationResult.SubjectFingerprint);
	OutResult.GrantedScopes = MoveTemp(AuthorizationResult.GrantedScopes);
	if (!OutResult.ValidateShape(ShapeError))
	{
		OutResult.Reset();
		const bool bLocalResolved =
			ReconcileAttemptedStore(Request.SecretHandle, ExpectedRevision, NewRevision, Encoded);
		return FailAfterAuthorization(MakeDurableAccountError(EUnrealAIErrorCategory::Provider,
															  EUnrealAIProviderAccessErrorCode::AuthResponseInvalid),
									  bLocalResolved);
	}
	if (!IsCommitCandidateCurrent(Request.Binding, AccountGeneration, Cancellation, Deadline))
	{
		OutResult.Reset();
		const bool bLocalResolved =
			ReconcileAttemptedStore(Request.SecretHandle, ExpectedRevision, NewRevision, Encoded);
		TerminalError = MakeDurableAccountObservedTerminalError(Cancellation, Deadline, *Clock);
		if (!TerminalError.IsError())
		{
			TerminalError = MakeDurableAccountError(EUnrealAIErrorCategory::Cancelled,
													EUnrealAIProviderAccessErrorCode::AuthCancelled);
		}
		return FailAfterAuthorization(TerminalError, bLocalResolved);
	}

	RevocationToken.Reset();
	OutError = {};
	return true;
}

bool FUnrealAIOAuthDurableAccountTransaction::TryCloneSecret(const FUnrealAISecretValue &Source,
															 FUnrealAISecretValue &OutClone)
{
	OutClone.Reset();
	if (!Source.IsSet())
	{
		return false;
	}
	TArray<uint8> Bytes;
	Bytes.Append(Source.View().GetData(), Source.View().Num());
	FString Error;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutClone, Error);
}

bool FUnrealAIOAuthDurableAccountTransaction::SecretsMatch(const FUnrealAISecretValue &A, const FUnrealAISecretValue &B)
{
	const TConstArrayView<uint8> ABytes = A.View();
	const TConstArrayView<uint8> BBytes = B.View();
	if (ABytes.Num() != BBytes.Num())
	{
		return false;
	}
	uint8 Difference = 0;
	for (int32 Index = 0; Index < ABytes.Num(); ++Index)
	{
		Difference |= ABytes[Index] ^ BBytes[Index];
	}
	return Difference == 0;
}

bool FUnrealAIOAuthDurableAccountTransaction::ReconcileAttemptedStore(const FUnrealAISecretHandle &Handle,
																	  const uint64 ExpectedRevision,
																	  const uint64 ReportedRevision,
																	  const FUnrealAISecretValue &AttemptedValue) const
{
	if (!AttemptedValue.IsSet() || (ReportedRevision == 0 && ExpectedRevision == TNumericLimits<uint64>::Max()))
	{
		return false;
	}
	const uint64 AttemptedRevision = ReportedRevision != 0 ? ReportedRevision : ExpectedRevision + 1;
	FUnrealAICancellationSource CleanupCancellation;
	FUnrealAISecretStoreOperationContext CleanupContext;
	FString ContextError;
	if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, FUnrealAISecretStoreOperationContext::MaxTimeoutSeconds,
														 CleanupCancellation.GetToken(), CleanupContext, ContextError))
	{
		return false;
	}

	FUnrealAISecretValue Observed;
	uint64 ObservedRevision = 0;
	FUnrealAIProviderAccessError StoreError;
	const EUnrealAISecretStoreResult LoadResult =
		SecretStore->Load(CleanupContext, Handle, Observed, ObservedRevision, StoreError);
	const bool bExactAttempt = LoadResult == EUnrealAISecretStoreResult::Succeeded &&
							   ObservedRevision == AttemptedRevision && SecretsMatch(AttemptedValue, Observed);
	Observed.Reset();
	if (LoadResult == EUnrealAISecretStoreResult::NotFound)
	{
		return true;
	}
	if (!bExactAttempt)
	{
		return false;
	}
	const EUnrealAISecretStoreResult DeleteResult =
		SecretStore->Delete(CleanupContext, Handle, AttemptedRevision, StoreError);
	return DeleteResult == EUnrealAISecretStoreResult::Succeeded ||
		   DeleteResult == EUnrealAISecretStoreResult::NotFound;
}

bool FUnrealAIOAuthDurableAccountTransaction::RevokeIssuedGrantToken(
	const FUnrealAIOAuthDurableAccountTransactionRequest &Request, FUnrealAISecretValue &&Token) const
{
	if (!Token.IsSet())
	{
		return false;
	}
	FUnrealAIOAuthRevocationRequest RevocationRequest;
	RevocationRequest.RequestId.Value = FGuid::NewGuid();
	RevocationRequest.Server = Request.AuthorizationRequest.Server;
	RevocationRequest.ClientId.Append(Request.AuthorizationRequest.ClientId);
	RevocationRequest.TimeoutSeconds = Request.CompensationTimeoutSeconds;
	FUnrealAICancellationSource RevocationCancellation;
	FUnrealAIProviderAccessError RevocationError;
	return Executor->Revoke(RevocationRequest, MoveTemp(Token), RevocationCancellation.GetToken(), RevocationError);
}

uint64 FUnrealAIOAuthDurableAccountTransaction::BeginAuthorization(const FUnrealAIOAuthTokenEnvelopeBinding &Binding)
{
	FScopeLock Lock(&FenceMutex);
	for (FAccountFenceRecord &Record : AccountFences)
	{
		if (Record.Binding == Binding)
		{
			++Record.Generation;
			if (Record.Generation == 0)
			{
				Record.Generation = 1;
			}
			++Record.ActiveOperations;
			return Record.Generation;
		}
	}
	if (AccountFences.Num() >= MaxActiveAccountFences)
	{
		return 0;
	}
	FAccountFenceRecord &Record = AccountFences.AddDefaulted_GetRef();
	Record.Binding = Binding;
	Record.Generation = 1;
	Record.ActiveOperations = 1;
	return Record.Generation;
}

void FUnrealAIOAuthDurableAccountTransaction::EndAuthorization(const FUnrealAIOAuthTokenEnvelopeBinding &Binding)
{
	FScopeLock Lock(&FenceMutex);
	for (int32 Index = 0; Index < AccountFences.Num(); ++Index)
	{
		FAccountFenceRecord &Record = AccountFences[Index];
		if (Record.Binding == Binding)
		{
			Record.ActiveOperations = FMath::Max(0, Record.ActiveOperations - 1);
			if (Record.ActiveOperations == 0)
			{
				AccountFences.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
			return;
		}
	}
}

bool FUnrealAIOAuthDurableAccountTransaction::IsCommitCandidateCurrent(
	const FUnrealAIOAuthTokenEnvelopeBinding &Binding, const uint64 Generation,
	const FUnrealAICancellationToken &Cancellation, const FUnrealAIDeadline &Deadline)
{
	FScopeLock Lock(&FenceMutex);
	if (Cancellation.IsCancellationRequested() || Deadline.IsExpired(*Clock))
	{
		return false;
	}
	for (const FAccountFenceRecord &Record : AccountFences)
	{
		if (Record.Binding == Binding)
		{
			return Record.Generation == Generation;
		}
	}
	return false;
}
