// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"

#include "Auth/UnrealAIMemorySecretStore.h"
#include "Auth/UnrealAIOAuthDurableAccountTransaction.h"
#include "Runtime/UnrealAIClock.h"

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

namespace
{
constexpr TCHAR DurableStoreName[] = TEXT("tests.oauth.durable.store");
const FUnrealAISecretHandle DurableSecretHandle{DurableStoreName,
												FGuid(0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f001)};

FUnrealAIProviderAccessError MakeDurableTestError(const EUnrealAIErrorCategory Category,
												  const EUnrealAIProviderAccessErrorCode Code,
												  const bool bRetryable = false)
{
	FUnrealAIProviderAccessError Error;
	Error.Category = Category;
	Error.Code = Code;
	Error.bRetryable = bRetryable;
	return Error;
}

bool MakeDurableTestSecret(const ANSICHAR *Text, FUnrealAISecretValue &OutSecret)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Text), FCStringAnsi::Strlen(Text));
	FString Error;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, Error);
}

FUnrealAIOAuthTrustedAuthorizationServer MakeDurableTestServer()
{
	FUnrealAIOAuthTrustedAuthorizationServer Server;
	Server.Issuer = TEXT("https://issuer.example.test");
	Server.DiscoveryEndpoint = TEXT("https://issuer.example.test/.well-known/openid-configuration");
	Server.AuthorizationEndpoint = TEXT("https://issuer.example.test/oauth2/authorize");
	Server.TokenEndpoint = TEXT("https://issuer.example.test/oauth2/token");
	Server.JwksEndpoint = TEXT("https://issuer.example.test/oauth2/jwks");
	Server.RevocationEndpoint = TEXT("https://issuer.example.test/oauth2/revoke");
	Server.AllowedSigningAlgorithms = {FName(TEXT("RS256"))};
	return Server;
}

FUnrealAIOAuthDurableAccountTransactionRequest MakeDurableTestRequest(const uint32 Seed)
{
	FUnrealAIOAuthDurableAccountTransactionRequest Request;
	Request.AuthorizationRequest.RequestId.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	Request.AuthorizationRequest.Server = MakeDurableTestServer();
	Request.AuthorizationRequest.ClientId = TEXT("durable-public-client");
	Request.AuthorizationRequest.Audience = TEXT("https://api.example.test/resource");
	Request.AuthorizationRequest.ExactRedirectUri = TEXT("http://127.0.0.1:43821/callback");
	Request.AuthorizationRequest.RequestedScopes = {TEXT("openid"), TEXT("profile"), TEXT("offline_access")};
	Request.AuthorizationRequest.TimeoutSeconds = 30.0;
	Request.AuthorizationRequest.ClockSkewSeconds = 30.0;
	Request.AuthorizationRequest.MaxIdentityTokenAgeSeconds = 900.0;
	Request.AuthorizationRequest.MaxJwksCacheAgeSeconds = 600.0;
	Request.Binding.ProviderName = TEXT("tests.oauth.durable.provider");
	Request.Binding.AuthProfileId = TEXT("tests.oauth.durable.profile");
	Request.Binding.AccountId.Value = FGuid(0xabcdef01, 0x23456789, 0x13572468, 0x24681357);
	Request.SecretHandle = DurableSecretHandle;
	return Request;
}

class FDurableTransactionTestExecutor final : public IUnrealAIOAuthAuthorizationExecutor
{
  public:
	explicit FDurableTransactionTestExecutor(TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> InClock)
		: Clock(MoveTemp(InClock))
	{
	}

	bool AuthorizeBrowserPkce(const FUnrealAIOAuthBrowserAuthorizationRequest &Request,
							  const FUnrealAICancellationToken &Cancellation,
							  FUnrealAIOAuthAuthorizationResult &OutResult,
							  FUnrealAIProviderAccessError &OutError) override
	{
		(void)Request;
		++AuthorizeCalls;
		OutResult.Reset();
		if (bAuthorizeFails)
		{
			OutError =
				MakeDurableTestError(EUnrealAIErrorCategory::Provider, EUnrealAIProviderAccessErrorCode::AuthFailed);
			return false;
		}
		if ((!bOmitAccessToken && !MakeDurableTestSecret("issued-access-token", OutResult.Tokens.AccessToken)) ||
			(!bOmitRefreshToken && !MakeDurableTestSecret("issued-refresh-token", OutResult.Tokens.RefreshToken)) ||
			!MakeDurableTestSecret("issued-id-token", OutResult.Tokens.IdToken))
		{
			OutResult.Reset();
			OutError = MakeDurableTestError(EUnrealAIErrorCategory::Memory,
											EUnrealAIProviderAccessErrorCode::SecretCopyFailed);
			return false;
		}
		OutResult.Tokens.AccessTokenExpiresAtUtc = Clock->UtcNow() + FTimespan::FromHours(1);
		OutResult.SubjectFingerprint = TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
		OutResult.GrantedScopes = {TEXT("openid"), TEXT("profile"), TEXT("offline_access")};
		if (OnAuthorized)
		{
			OnAuthorized();
		}
		OutError = {};
		return true;
	}

	bool Revoke(const FUnrealAIOAuthRevocationRequest &Request, FUnrealAISecretValue &&Token,
				const FUnrealAICancellationToken &Cancellation, FUnrealAIProviderAccessError &OutError) override
	{
		++RevokeCalls;
		bSawRevocationToken = Token.IsSet();
		RevocationTokenBytes = Token.Num();
		bSawFreshCancellation = !Cancellation.IsCancellationRequested();
		bSawExactRevocationAuthority =
			Request.Server.RevocationEndpoint == TEXT("https://issuer.example.test/oauth2/revoke") &&
													  Request.ClientId == TEXT("durable-public-client") &&
																			   Request.TimeoutSeconds == 60.0;
		Token.Reset();
		if (bRevokeFails)
		{
			OutError = MakeDurableTestError(EUnrealAIErrorCategory::Provider,
											EUnrealAIProviderAccessErrorCode::AuthFailed, true);
			return false;
		}
		OutError = {};
		return true;
	}

	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TFunction<void()> OnAuthorized;
	bool bAuthorizeFails = false;
	bool bOmitAccessToken = false;
	bool bOmitRefreshToken = false;
	bool bRevokeFails = false;
	int32 AuthorizeCalls = 0;
	int32 RevokeCalls = 0;
	bool bSawRevocationToken = false;
	int32 RevocationTokenBytes = 0;
	bool bSawFreshCancellation = false;
	bool bSawExactRevocationAuthority = false;
};

enum class EDurableStoreBehavior : uint8
{
	Normal,
	FailBeforeCommit,
	ReportCancelledAfterCommit,
	CancelAfterCommit,
	DeleteAndCancelAfterCommit,
	InstallWinnerAndCancelAfterCommit
};

class FDurableTransactionTestStore final : public IUnrealAISecretStore
{
  public:
	FDurableTransactionTestStore()
		: Inner(MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(DurableStoreName, 8))
	{
	}

	FName GetStoreName() const override
	{
		return DurableStoreName;
	}

	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override
	{
		FUnrealAISecretStoreCapabilities Capabilities;
		Capabilities.PersistenceClass = EUnrealAISecretStorePersistenceClass::Persistent;
		Capabilities.ProtectionClass = EUnrealAISecretStoreProtectionClass::PlatformCredentialStore;
		Capabilities.ScopeClass = EUnrealAISecretStoreScopeClass::CurrentUser;
		Capabilities.bAvailableInCurrentBuild = true;
		Capabilities.bPersistent = true;
		Capabilities.bAtomicCompareAndSwap = true;
		Capabilities.bAvailableInShipping = false;
		return Capabilities;
	}

	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &Context,
									const FUnrealAISecretHandle &Handle, FUnrealAISecretValue &OutValue,
									uint64 &OutRevision, FUnrealAIProviderAccessError &OutError) override
	{
		++LoadCalls;
		return Inner->Load(Context, Handle, OutValue, OutRevision, OutError);
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &Context,
									 const FUnrealAISecretHandle &Handle, const FUnrealAISecretValue &Value,
									 const uint64 ExpectedRevision, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		++StoreCalls;
		if (Behavior == EDurableStoreBehavior::FailBeforeCommit)
		{
			OutNewRevision = 0;
			OutError = MakeDurableTestError(EUnrealAIErrorCategory::Persistence,
											EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, true);
			return EUnrealAISecretStoreResult::Unavailable;
		}

		const EUnrealAISecretStoreResult InnerResult =
			Inner->Store(Context, Handle, Value, ExpectedRevision, OutNewRevision, OutError);
		if (InnerResult != EUnrealAISecretStoreResult::Succeeded)
		{
			return InnerResult;
		}
		const uint64 CommittedRevision = OutNewRevision;
		if (Behavior == EDurableStoreBehavior::ReportCancelledAfterCommit)
		{
			OutNewRevision = 0;
			OutError = MakeDurableTestError(EUnrealAIErrorCategory::Cancelled,
											EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
			return EUnrealAISecretStoreResult::Cancelled;
		}
		if (Behavior == EDurableStoreBehavior::DeleteAndCancelAfterCommit)
		{
			FUnrealAIProviderAccessError DeleteError;
			Inner->Delete(Context, Handle, CommittedRevision, DeleteError);
		}
		else if (Behavior == EDurableStoreBehavior::InstallWinnerAndCancelAfterCommit)
		{
			FUnrealAISecretValue Winner;
			MakeDurableTestSecret("later-writer-wins", Winner);
			uint64 WinnerRevision = 0;
			FUnrealAIProviderAccessError WinnerError;
			Inner->Store(Context, Handle, Winner, CommittedRevision, WinnerRevision, WinnerError);
			Winner.Reset();
		}
		if (Behavior == EDurableStoreBehavior::CancelAfterCommit ||
			Behavior == EDurableStoreBehavior::DeleteAndCancelAfterCommit ||
			Behavior == EDurableStoreBehavior::InstallWinnerAndCancelAfterCommit)
		{
			if (OnCommitted)
			{
				OnCommitted();
			}
		}
		return InnerResult;
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &Context,
									  const FUnrealAISecretHandle &Handle, const uint64 ExpectedRevision,
									  FUnrealAIProviderAccessError &OutError) override
	{
		++DeleteCalls;
		return Inner->Delete(Context, Handle, ExpectedRevision, OutError);
	}

	int32 Num() const
	{
		return Inner->Num();
	}

	EDurableStoreBehavior Behavior = EDurableStoreBehavior::Normal;
	TFunction<void()> OnCommitted;
	int32 LoadCalls = 0;
	int32 StoreCalls = 0;
	int32 DeleteCalls = 0;

  private:
	TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Inner;
};

struct FDurableTransactionFixture
{
	FDurableTransactionFixture()
		: MutableClock(MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 8, 23, 12, 0, 0), 10.0)),
		  Clock(MutableClock), Store(MakeShared<FDurableTransactionTestStore, ESPMode::ThreadSafe>()),
		  Executor(MakeShared<FDurableTransactionTestExecutor, ESPMode::ThreadSafe>(Clock)),
		  Transaction(MakeUnique<FUnrealAIOAuthDurableAccountTransaction>(Executor, Store, Clock))
	{
	}

	bool Run(const uint32 Seed, FUnrealAIProviderAccessError &OutError)
	{
		Result.Reset();
		return Transaction->AuthorizeAndCommit(MakeDurableTestRequest(Seed), Cancellation.GetToken(), Result, OutError);
	}

	TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock;
	TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock;
	TSharedRef<FDurableTransactionTestStore, ESPMode::ThreadSafe> Store;
	TSharedRef<FDurableTransactionTestExecutor, ESPMode::ThreadSafe> Executor;
	TUniquePtr<FUnrealAIOAuthDurableAccountTransaction> Transaction;
	FUnrealAICancellationSource Cancellation;
	FUnrealAIOAuthDurableAccountCommitResult Result;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountCommitSuccessTest,
								 "UnrealAI.Auth.OAuthDurableAccount.CommitSuccess",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountCommitSuccessTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FDurableTransactionFixture Fixture;
	FUnrealAIProviderAccessError Error;
	TestTrue(TEXT("Production protected store is admitted"), Fixture.Transaction->IsAvailable());
	FUnrealAICancellationToken InvalidCancellation;
	FUnrealAIOAuthDurableAccountCommitResult InvalidResult;
	TestFalse(TEXT("Invalid cancellation lineage fails before authorization"),
				   Fixture.Transaction->AuthorizeAndCommit(MakeDurableTestRequest(99), InvalidCancellation,
														   InvalidResult, Error));
	TestEqual(TEXT("Invalid cancellation lineage is a typed request fault"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::InvalidRequest);
	TestEqual(TEXT("Invalid cancellation lineage starts no authorization"), Fixture.Executor->AuthorizeCalls, 0);
	TestTrue(TEXT("Authorization commits atomically"), Fixture.Run(100, Error));
	TestEqual(TEXT("One authorization ran"), Fixture.Executor->AuthorizeCalls, 1);
	TestEqual(TEXT("Success does not revoke the committed credential"), Fixture.Executor->RevokeCalls, 0);
	TestEqual(TEXT("One protected record remains"), Fixture.Store->Num(), 1);
	FString ShapeError;
	TestTrue(TEXT("Receipt has a valid non-secret shape"), Fixture.Result.ValidateShape(ShapeError));
	TestEqual(TEXT("Commit receipt reports the winning CAS revision"), Fixture.Result.SecretRevision, uint64(1));
	TestFalse(TEXT("Successful output error is empty"), Error.IsError());
	FUnrealAICancellationSource InspectionCancellation;
	FUnrealAISecretStoreOperationContext InspectionContext;
	TestTrue(TEXT("Inspection context is valid"),
				  FUnrealAISecretStoreOperationContext::TryCreate(Fixture.Clock, 5.0, InspectionCancellation.GetToken(),
																  InspectionContext, ShapeError));
	FUnrealAISecretValue Stored;
	uint64 StoredRevision = 0;
	FUnrealAIProviderAccessError StoreError;
	TestEqual(TEXT("Committed envelope loads from the protected store"),
				   Fixture.Store->Load(InspectionContext, DurableSecretHandle, Stored, StoredRevision, StoreError),
				   EUnrealAISecretStoreResult::Succeeded);
	FUnrealAIOAuthTokenEnvelope Envelope;
	TestTrue(TEXT("Committed bytes decode under the exact account binding"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryDecode(MoveTemp(Stored), MakeDurableTestRequest(100).Binding,
															  Fixture.Clock->UtcNow(), Envelope, ShapeError));
	TestTrue(TEXT("Durable envelope retains refresh credentials"), Envelope.HasRefreshToken());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountRejectsVolatileStoreTest,
								 "UnrealAI.Auth.OAuthDurableAccount.RejectsVolatileStore",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountRejectsVolatileStoreTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 8, 23, 12, 0, 0), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Store =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(DurableStoreName, 8);
	const TSharedRef<FDurableTransactionTestExecutor, ESPMode::ThreadSafe> Executor =
		MakeShared<FDurableTransactionTestExecutor, ESPMode::ThreadSafe>(Clock);
	FUnrealAIOAuthDurableAccountTransaction Transaction(Executor, Store, Clock);
	FUnrealAICancellationSource Cancellation;
	FUnrealAIOAuthDurableAccountCommitResult Result;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Volatile process memory is not a durable account store"), Transaction.IsAvailable());
	TestFalse(TEXT("Transaction fails before authorization on a volatile store"),
				   Transaction.AuthorizeAndCommit(MakeDurableTestRequest(150), Cancellation.GetToken(), Result, Error));
	TestEqual(TEXT("Unsupported store is typed"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
	TestEqual(TEXT("No authorization runs without a production store"), Executor->AuthorizeCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountPersistenceFailureTest,
								 "UnrealAI.Auth.OAuthDurableAccount.PersistenceFailureRevokes",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountPersistenceFailureTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FDurableTransactionFixture Fixture;
	Fixture.Store->Behavior = EDurableStoreBehavior::FailBeforeCommit;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Failed persistence fails the transaction"), Fixture.Run(200, Error));
	TestEqual(TEXT("Failure is typed as durable persistence"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed);
	TestEqual(TEXT("Issued credential is revoked exactly once"), Fixture.Executor->RevokeCalls, 1);
	TestTrue(TEXT("Revocation receives a protected token"), Fixture.Executor->bSawRevocationToken);
	TestEqual(TEXT("Revocation prefers the issued refresh credential"), Fixture.Executor->RevocationTokenBytes,
				   FCStringAnsi::Strlen("issued-refresh-token"));
	TestTrue(TEXT("Revocation uses a fresh uncancelled lineage"), Fixture.Executor->bSawFreshCancellation);
	TestTrue(TEXT("Revocation reuses exact trusted issuer authority"), Fixture.Executor->bSawExactRevocationAuthority);
	TestEqual(TEXT("No credential remains locally"), Fixture.Store->Num(), 0);

	FDurableTransactionFixture MalformedFixture;
	MalformedFixture.Executor->bOmitAccessToken = true;
	TestFalse(TEXT("A malformed refresh-only success fails closed"), MalformedFixture.Run(201, Error));
	TestEqual(TEXT("Malformed refresh-only success is typed"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthResponseInvalid);
	TestEqual(TEXT("Malformed refresh-only grant is revoked"), MalformedFixture.Executor->RevokeCalls, 1);
	TestEqual(TEXT("Malformed success revokes its refresh credential"), MalformedFixture.Executor->RevocationTokenBytes,
				   FCStringAnsi::Strlen("issued-refresh-token"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountBindingMismatchTest,
								 "UnrealAI.Auth.OAuthDurableAccount.BindingMismatchPreservesExisting",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountBindingMismatchTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FDurableTransactionFixture Fixture;
	FUnrealAIOAuthTokenSet ExistingTokens;
	MakeDurableTestSecret("other-access-token", ExistingTokens.AccessToken);
	MakeDurableTestSecret("other-refresh-token", ExistingTokens.RefreshToken);
	MakeDurableTestSecret("other-id-token", ExistingTokens.IdToken);
	ExistingTokens.AccessTokenExpiresAtUtc = Fixture.Clock->UtcNow() + FTimespan::FromHours(1);
	FUnrealAIOAuthTokenEnvelopeBinding OtherBinding = MakeDurableTestRequest(250).Binding;
	OtherBinding.AccountId.Value = FGuid(0x01010101, 0x02020202, 0x03030303, 0x04040404);
	FUnrealAIOAuthTokenEnvelope ExistingEnvelope;
	FUnrealAISecretValue EncodedExisting;
	FString ShapeError;
	TestTrue(TEXT("Mismatched account envelope is created"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryCreate(OtherBinding, MoveTemp(ExistingTokens),
															  Fixture.Clock->UtcNow(), ExistingEnvelope, ShapeError));
	TestTrue(TEXT("Mismatched account envelope is encoded"),
				  FUnrealAIOAuthTokenEnvelopeCodec::TryEncode(MoveTemp(ExistingEnvelope), Fixture.Clock->UtcNow(),
															  EncodedExisting, ShapeError));
	FUnrealAICancellationSource SetupCancellation;
	FUnrealAISecretStoreOperationContext SetupContext;
	TestTrue(TEXT("Setup store context is valid"),
				  FUnrealAISecretStoreOperationContext::TryCreate(Fixture.Clock, 5.0, SetupCancellation.GetToken(),
																  SetupContext, ShapeError));
	uint64 ExistingRevision = 0;
	FUnrealAIProviderAccessError SetupError;
	TestEqual(
		TEXT("Mismatched account record is prepopulated"),
			 Fixture.Store->Store(SetupContext, DurableSecretHandle, EncodedExisting, 0, ExistingRevision, SetupError),
			 EUnrealAISecretStoreResult::Succeeded);
	EncodedExisting.Reset();

	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Transaction rejects a handle bound to another account"), Fixture.Run(250, Error));
	TestEqual(TEXT("Binding mismatch fails the durable commit"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed);
	TestEqual(TEXT("No replacement write occurs"), Fixture.Store->StoreCalls, 1);
	TestEqual(TEXT("Existing account record remains intact"), Fixture.Store->Num(), 1);
	TestEqual(TEXT("Newly issued credential is revoked"), Fixture.Executor->RevokeCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountAmbiguousCommitTest,
								 "UnrealAI.Auth.OAuthDurableAccount.AmbiguousCommitDeletesAndRevokes",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountAmbiguousCommitTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FDurableTransactionFixture Fixture;
	Fixture.Store->Behavior = EDurableStoreBehavior::ReportCancelledAfterCommit;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Ambiguous post-commit result fails closed"), Fixture.Run(300, Error));
	TestEqual(TEXT("Ambiguous commit is a persistence failure"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed);
	TestEqual(TEXT("Exact attempted record is removed"), Fixture.Store->Num(), 0);
	TestEqual(TEXT("Exact cleanup performs one delete"), Fixture.Store->DeleteCalls, 1);
	TestEqual(TEXT("Issued credential is also revoked"), Fixture.Executor->RevokeCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountCancellationAfterAuthorizationTest,
								 "UnrealAI.Auth.OAuthDurableAccount.CancellationAfterAuthorizationRevokes",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountCancellationAfterAuthorizationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FDurableTransactionFixture Fixture;
	Fixture.Executor->OnAuthorized = [&Fixture]()
	{ Fixture.Cancellation.Cancel(EUnrealAICancellationReason::Requested); };
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Cancellation fences the issued authorization"), Fixture.Run(400, Error));
	TestEqual(TEXT("Cancellation remains the terminal reason after successful compensation"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthCancelled);
	TestEqual(TEXT("Cancellation prevents a store write"), Fixture.Store->StoreCalls, 0);
	TestEqual(TEXT("Issued credential is revoked"), Fixture.Executor->RevokeCalls, 1);
	TestTrue(TEXT("Compensating revoke ignores the cancelled parent lineage"), Fixture.Executor->bSawFreshCancellation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountSignOutRaceTest,
								 "UnrealAI.Auth.OAuthDurableAccount.SignOutRaceDeletesAndRevokes",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountSignOutRaceTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FDurableTransactionFixture Fixture;
	Fixture.Store->Behavior = EDurableStoreBehavior::CancelAfterCommit;
	Fixture.Store->OnCommitted = [&Fixture]()
	{ Fixture.Transaction->InvalidateAccountForSignOut(MakeDurableTestRequest(500).Binding); };
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Sign-out cancellation wins the commit race"), Fixture.Run(500, Error));
	TestEqual(TEXT("Resolved sign-out race remains cancelled"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthCancelled);
	TestEqual(TEXT("Compensation leaves no revived local credential"), Fixture.Store->Num(), 0);
	TestEqual(TEXT("The issued credential is revoked after sign-out wins"), Fixture.Executor->RevokeCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountLaterWriterWinsTest,
								 "UnrealAI.Auth.OAuthDurableAccount.LaterWriterWins",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountLaterWriterWinsTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FDurableTransactionFixture Fixture;
	Fixture.Store->Behavior = EDurableStoreBehavior::InstallWinnerAndCancelAfterCommit;
	Fixture.Store->OnCommitted = [&Fixture]() { Fixture.Cancellation.Cancel(EUnrealAICancellationReason::Requested); };
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Stale transaction does not claim success"), Fixture.Run(600, Error));
	TestEqual(TEXT("Unresolved local replacement is surfaced as persistence failure"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed);
	TestEqual(TEXT("Later writer remains intact"), Fixture.Store->Num(), 1);
	TestEqual(TEXT("Transaction never deletes a later CAS revision"), Fixture.Store->DeleteCalls, 0);
	TestEqual(TEXT("Stale issued credential is revoked"), Fixture.Executor->RevokeCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOAuthDurableAccountRevocationFailureTest,
								 "UnrealAI.Auth.OAuthDurableAccount.RevocationFailureFailsClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIOAuthDurableAccountRevocationFailureTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FDurableTransactionFixture Fixture;
	Fixture.Executor->OnAuthorized = [&Fixture]()
	{ Fixture.Cancellation.Cancel(EUnrealAICancellationReason::Requested); };
	Fixture.Executor->bRevokeFails = true;
	FUnrealAIProviderAccessError Error;
	TestFalse(TEXT("Failed compensation never returns success"), Fixture.Run(700, Error));
	TestEqual(TEXT("Unresolved remote compensation is a retryable persistence failure"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::AuthPersistenceFailed);
	TestTrue(TEXT("Unresolved compensation is retryable"), Error.bRetryable);
	TestEqual(TEXT("Revocation was still attempted exactly once"), Fixture.Executor->RevokeCalls, 1);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
