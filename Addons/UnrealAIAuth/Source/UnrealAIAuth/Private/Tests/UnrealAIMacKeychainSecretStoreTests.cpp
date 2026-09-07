// Copyright EngineWorks. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Testing/UnrealAITestClock.h"

#include "Auth/UnrealAIMacKeychainSecretStore.h"
#include "Runtime/UnrealAICancellation.h"
#include "Runtime/UnrealAIClock.h"

#if defined(WITH_AUTOMATION_TESTS) && WITH_AUTOMATION_TESTS

namespace
{
bool MakeFixtureSecret(const uint8 Seed, FUnrealAISecretValue &OutSecret)
{
	TArray<uint8> Bytes{Seed, static_cast<uint8>(Seed + 1), static_cast<uint8>(Seed + 2), static_cast<uint8>(Seed + 3)};
	FString Error;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, Error);
}

FUnrealAICredentialDestination MakeFixtureDestination()
{
	FUnrealAICredentialDestination Destination;
	Destination.ModelProviderName = TEXT("tests.keychain.provider");
	Destination.AccountAuthProviderName = TEXT("tests.keychain.auth");
	Destination.AuthProfileId = TEXT("tests.keychain.profile");
	Destination.AccountId.Value = FGuid(1, 2, 3, 4);
	Destination.TenantRealm = TEXT("tests.local_machine");
	Destination.BillingPrincipalId.Value = FGuid(5, 6, 7, 8);
	Destination.PayerHandle = TEXT("tests.keychain.payer");
	Destination.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Destination.BillingMode = EUnrealAIBillingMode::ApiMetered;
	Destination.Audience = TEXT("https://keychain.invalid/v1/test");
	Destination.ConnectionRevision = 1;
	Destination.EndpointPolicyRevision = 1;
	FString Error;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://keychain.invalid"), false, Destination.EndpointOrigin, Error);
	return Destination;
}

class FFixtureSecretApplicator final : public IUnrealAICredentialApplicator
{
  public:
	FFixtureSecretApplicator(FUnrealAICredentialDestination InDestination, const uint8 InSeed)
		: Destination(MoveTemp(InDestination)), Seed(InSeed)
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	bool bMatched = false;

  protected:
	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme Scheme, const TConstArrayView<uint8> Secret) override
	{
		uint8 Difference = Secret.Num() == 4 ? 0 : 1;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const uint8 Actual = Secret.IsValidIndex(Index) ? Secret[Index] : 0;
			Difference |= Actual ^ static_cast<uint8>(Seed + Index);
		}
		bMatched = Scheme == EUnrealAIAuthScheme::ApiKey && Difference == 0;
		return bMatched;
	}

	bool DispatchWithoutCredential() override
	{
		return false;
	}

  private:
	FUnrealAICredentialDestination Destination;
	uint8 Seed = 0;
};

bool VerifyFixtureSecret(FUnrealAISecretValue &&Secret, const uint8 Seed,
						 const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> &Clock)
{
	const FUnrealAICredentialDestination Destination = MakeFixtureDestination();
	FUnrealAICredentialFreshnessSource Freshness;
	FUnrealAICredentialLease Lease;
	FString Error;
	if (!FUnrealAICredentialLease::TryCreate(Destination, Clock, Freshness.GetToken(), 10.0, {}, MoveTemp(Secret),
											 Lease, Error))
	{
		return false;
	}
	FFixtureSecretApplicator Applicator(Destination, Seed);
	return Lease.TryApplyTo(Applicator, Error) && Applicator.bMatched;
}

#if PLATFORM_MAC
class FKeychainRecordCleanup final
{
  public:
	FKeychainRecordCleanup(TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> InStore,
						   const FUnrealAISecretStoreOperationContext &InContext, const FUnrealAISecretHandle &InHandle)
		: Store(MoveTemp(InStore)), Context(InContext), Handle(InHandle)
	{
	}

	~FKeychainRecordCleanup()
	{
		FUnrealAISecretValue Existing;
		uint64 Revision = 0;
		FUnrealAIProviderAccessError Error;
		if (Store->Load(Context, Handle, Existing, Revision, Error) == EUnrealAISecretStoreResult::Succeeded &&
			Revision != 0)
		{
			Existing.Reset();
			Store->Delete(Context, Handle, Revision, Error);
		}
		Existing.Reset();
	}

  private:
	TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store;
	FUnrealAISecretStoreOperationContext Context;
	FUnrealAISecretHandle Handle;
};
#endif
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIMacKeychainSecretStoreConformanceTest,
								 "UnrealAI.Integration.Auth.MacKeychainConformance",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIMacKeychainSecretStoreFaultMappingTest,
								 "UnrealAI.Auth.MacKeychainFaultMapping",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIMacKeychainSecretStoreFaultMappingTest::RunTest(const FString &Parameters)
{

	(void)Parameters;
#if PLATFORM_MAC
	const TSharedRef<FUnrealAIMacKeychainSecretStore, ESPMode::ThreadSafe> Store =
		MakeShared<FUnrealAIMacKeychainSecretStore, ESPMode::ThreadSafe>();
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	FString ContextError;
	if (!TestTrue(TEXT("Fault-mapping context constructs"),
					   FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context,
																	   ContextError)))
	{
		return false;
	}
	const FUnrealAISecretHandle Handle{FUnrealAIMacKeychainSecretStore::StoreName(), FGuid::NewGuid()};

	using EPlatformStatus = FUnrealAIMacKeychainSecretStore::EAutomationPlatformStatus;
	struct FFaultCase final
	{
		const TCHAR *Label = nullptr;
		EPlatformStatus PlatformStatus = EPlatformStatus::Succeeded;
		EUnrealAISecretStoreResult ExpectedResult = EUnrealAISecretStoreResult::Failed;
		EUnrealAIErrorCategory ExpectedCategory = EUnrealAIErrorCategory::None;
		EUnrealAIProviderAccessErrorCode ExpectedCode = EUnrealAIProviderAccessErrorCode::None;
		bool bExpectedRetryable = false;
	};
	const FFaultCase Cases[] = {
		{TEXT("interaction-not-allowed"), EPlatformStatus::InteractionNotAllowed, EUnrealAISecretStoreResult::Locked,
			  EUnrealAIErrorCategory::Busy, EUnrealAIProviderAccessErrorCode::SecretStoreLocked, true},
		 {TEXT("authentication-failed"), EPlatformStatus::AuthFailed, EUnrealAISecretStoreResult::Denied,
			   EUnrealAIErrorCategory::PolicyDenied, EUnrealAIProviderAccessErrorCode::SecretStoreDenied, false},
		  {TEXT("not-available"), EPlatformStatus::NotAvailable, EUnrealAISecretStoreResult::Unavailable,
				EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, true},
		   {TEXT("decode"), EPlatformStatus::Decode, EUnrealAISecretStoreResult::Corrupt,
				 EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt, false},
		  };

	for (const FFaultCase &Fault : Cases)
	{
		Store->SetAutomationPlatformCallOverride(Fault.PlatformStatus);
		FUnrealAISecretValue Loaded;
		uint64 Revision = 99;
		FUnrealAIProviderAccessError Error;
		const EUnrealAISecretStoreResult Result = Store->Load(Context, Handle, Loaded, Revision, Error);
		TestEqual(*FString::Printf(TEXT("%s maps to its closed store result"), Fault.Label), Result,
								   Fault.ExpectedResult);
		TestEqual(*FString::Printf(TEXT("%s maps to its closed error category"), Fault.Label), Error.Category,
								   Fault.ExpectedCategory);
		TestEqual(*FString::Printf(TEXT("%s maps to its closed error code"), Fault.Label), Error.Code,
								   Fault.ExpectedCode);
		TestEqual(*FString::Printf(TEXT("%s maps to the exact retry policy"), Fault.Label), Error.bRetryable,
								   Fault.bExpectedRetryable);
		TestEqual(*FString::Printf(TEXT("%s does not invent a retry delay"), Fault.Label), Error.RetryAfterSeconds,
								   0.0f);
		TestFalse(*FString::Printf(TEXT("%s never returns secret bytes"), Fault.Label), Loaded.IsSet());
		TestEqual(*FString::Printf(TEXT("%s never returns a revision"), Fault.Label), Revision, uint64{0});
		FString ErrorShape;
		TestTrue(*FString::Printf(TEXT("%s produces a valid public error envelope"), Fault.Label),
								  Error.ValidateShape(ErrorShape));
	}

	FUnrealAICancellationSource LoadCancellation;
	FUnrealAISecretStoreOperationContext CancelDuringLoadContext;
	TestTrue(TEXT("Load cancellation context constructs before the platform call"),
				  FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, LoadCancellation.GetToken(),
																  CancelDuringLoadContext, ContextError));
	bool bLoadPlatformCallReturned = false;
	Store->SetAutomationPlatformCallOverride(EPlatformStatus::Succeeded,
											 [&LoadCancellation, &bLoadPlatformCallReturned]()
											 {
												 bLoadPlatformCallReturned = true;
												 LoadCancellation.Cancel(EUnrealAICancellationReason::Requested);
											 });
	FUnrealAISecretValue Loaded;
	uint64 Revision = 99;
	FUnrealAIProviderAccessError Error;
	TestEqual(TEXT("Cancellation requested as the load platform call returns wins over its status"),
				   Store->Load(CancelDuringLoadContext, Handle, Loaded, Revision, Error),
				   EUnrealAISecretStoreResult::Cancelled);
	TestTrue(TEXT("Load cancellation was injected after the platform-call boundary"), bLoadPlatformCallReturned);
	TestEqual(TEXT("Post-call load cancellation has its exact category"), Error.Category,
				   EUnrealAIErrorCategory::Cancelled);
	TestEqual(TEXT("Post-call load cancellation has its exact code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
	TestFalse(TEXT("Post-call load cancellation is not retryable"), Error.bRetryable);
	TestFalse(TEXT("Post-call cancelled load returns no secret"), Loaded.IsSet());
	TestEqual(TEXT("Post-call cancelled load returns no revision"), Revision, uint64{0});

	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableStoreClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 20.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> StoreClock = MutableStoreClock;
	FUnrealAICancellationSource StoreCancellation;
	FUnrealAISecretStoreOperationContext TimeoutDuringStoreContext;
	TestTrue(TEXT("Store timeout context constructs before the platform call"),
				  FUnrealAISecretStoreOperationContext::TryCreate(StoreClock, 1.0, StoreCancellation.GetToken(),
																  TimeoutDuringStoreContext, ContextError));
	bool bStorePlatformCallReturned = false;
	Store->SetAutomationPlatformCallOverride(EPlatformStatus::Succeeded,
											 [MutableStoreClock, &bStorePlatformCallReturned]()
											 {
												 bStorePlatformCallReturned = true;
												 MutableStoreClock->Advance(FTimespan::FromSeconds(2.0));
											 });
	FUnrealAISecretValue Fixture;
	if (!TestTrue(TEXT("Post-call timeout fixture is guarded"), MakeFixtureSecret(53, Fixture)))
	{
		return false;
	}
	uint64 NewRevision = 99;
	TestEqual(TEXT("Timeout reached as the store platform call returns wins over its status"),
				   Store->Store(TimeoutDuringStoreContext, Handle, Fixture, 0, NewRevision, Error),
				   EUnrealAISecretStoreResult::TimedOut);
	Fixture.Reset();
	TestTrue(TEXT("Store timeout was injected after the platform-call boundary"), bStorePlatformCallReturned);
	TestEqual(TEXT("Post-call store timeout has its exact category"), Error.Category, EUnrealAIErrorCategory::Timeout);
	TestEqual(TEXT("Post-call store timeout has its exact code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut);
	TestTrue(TEXT("Post-call store timeout remains retryable"), Error.bRetryable);
	TestEqual(TEXT("Post-call timed-out store publishes no revision"), NewRevision, uint64{0});

	FUnrealAICancellationSource DeleteCancellation;
	FUnrealAISecretStoreOperationContext CancelDuringDeleteContext;
	TestTrue(TEXT("Delete cancellation context constructs before the platform call"),
				  FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, DeleteCancellation.GetToken(),
																  CancelDuringDeleteContext, ContextError));
	bool bDeletePlatformCallReturned = false;
	Store->SetAutomationPlatformCallOverride(EPlatformStatus::Succeeded,
											 [&DeleteCancellation, &bDeletePlatformCallReturned]()
											 {
												 bDeletePlatformCallReturned = true;
												 DeleteCancellation.Cancel(EUnrealAICancellationReason::Requested);
											 });
	TestEqual(TEXT("Cancellation requested as the delete platform call returns wins over its status"),
				   Store->Delete(CancelDuringDeleteContext, Handle, 1, Error), EUnrealAISecretStoreResult::Cancelled);
	TestTrue(TEXT("Delete cancellation was injected after the platform-call boundary"), bDeletePlatformCallReturned);
	TestEqual(TEXT("Post-call delete cancellation has its exact category"), Error.Category,
				   EUnrealAIErrorCategory::Cancelled);
	TestEqual(TEXT("Post-call delete cancellation has its exact code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);
	TestFalse(TEXT("Post-call delete cancellation is not retryable"), Error.bRetryable);
#else
	TestFalse(TEXT("Fault injection is unavailable with the non-macOS adapter"),
				   FUnrealAIMacKeychainSecretStore::IsPlatformSupported());
#endif
	return true;
}

bool FUnrealAIMacKeychainSecretStoreConformanceTest::RunTest(const FString &Parameters)
{
#if PLATFORM_MAC
	if (!FParse::Param(FCommandLine::Get(), TEXT("UnrealAILiveKeychain")))
	{
		AddInfo(TEXT("Platform-store integration is opt-in with -UnrealAILiveKeychain; offline fixtures cover store contracts."));
		return true;
	}
#endif
	(void)Parameters;
	const TSharedRef<FUnrealAIMacKeychainSecretStore, ESPMode::ThreadSafe> Store =
		MakeShared<FUnrealAIMacKeychainSecretStore, ESPMode::ThreadSafe>();
	const FUnrealAISecretStoreCapabilities Capabilities = Store->DescribeCapabilities();
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	FString ContextError;
	if (!TestTrue(TEXT("Keychain test context is valid"),
					   FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context,
																	   ContextError)))
	{
		return false;
	}
	const FUnrealAISecretHandle Handle{FUnrealAIMacKeychainSecretStore::StoreName(), FGuid::NewGuid()};

#if PLATFORM_MAC
	TestTrue(TEXT("macOS Keychain is persistent"), Capabilities.bPersistent);
	TestTrue(TEXT("macOS Keychain supports revision compare-and-swap"), Capabilities.bAtomicCompareAndSwap);
	TestTrue(TEXT("macOS Keychain is available to Shipping runtime code"), Capabilities.bAvailableInShipping);
	FKeychainRecordCleanup Cleanup(Store, Context, Handle);

	FUnrealAISecretValue Loaded;
	uint64 LoadedRevision = 0;
	FUnrealAIProviderAccessError Error;
	if (!TestEqual(TEXT("Randomized Keychain record starts absent"),
						Store->Load(Context, Handle, Loaded, LoadedRevision, Error),
						EUnrealAISecretStoreResult::NotFound))
	{
		return false;
	}
	TestFalse(TEXT("Absent Keychain load returns no secret"), Loaded.IsSet());
	TestEqual(TEXT("Absent Keychain load returns revision zero"), LoadedRevision, uint64{0});

	FUnrealAISecretValue First;
	if (!TestTrue(TEXT("First noncredential fixture is guarded"), MakeFixtureSecret(17, First)))
	{
		return false;
	}
	uint64 FirstRevision = 0;
	if (!TestEqual(TEXT("Keychain creates an absent record with revision zero precondition"),
						Store->Store(Context, Handle, First, 0, FirstRevision, Error),
						EUnrealAISecretStoreResult::Succeeded))
	{
		return false;
	}
	TestTrue(TEXT("Created Keychain record has a nonzero revision"), FirstRevision != 0);
	First.Reset();

	if (!TestEqual(TEXT("Created Keychain record loads"), Store->Load(Context, Handle, Loaded, LoadedRevision, Error),
						EUnrealAISecretStoreResult::Succeeded))
	{
		return false;
	}
	TestTrue(TEXT("Created Keychain record returns guarded bytes"), Loaded.IsSet());
	TestEqual(TEXT("Created fixture length is preserved"), Loaded.Num(), 4);
	TestEqual(TEXT("Created revision round-trips"), LoadedRevision, FirstRevision);
	TestTrue(TEXT("Created fixture bytes round-trip exactly through the one-shot credential seam"),
				  VerifyFixtureSecret(MoveTemp(Loaded), 17, Clock));

	FUnrealAISecretValue Replacement;
	if (!TestTrue(TEXT("Replacement noncredential fixture is guarded"), MakeFixtureSecret(29, Replacement)))
	{
		return false;
	}
	uint64 ReplacementRevision = 0;
	if (!TestEqual(TEXT("Current revision replaces the Keychain record"),
						Store->Store(Context, Handle, Replacement, FirstRevision, ReplacementRevision, Error),
						EUnrealAISecretStoreResult::Succeeded))
	{
		return false;
	}
	TestTrue(TEXT("Replacement advances the Keychain revision"), ReplacementRevision > FirstRevision);

	uint64 RejectedRevision = 0;
	TestEqual(TEXT("Stale write revision fails closed"),
				   Store->Store(Context, Handle, Replacement, FirstRevision, RejectedRevision, Error),
				   EUnrealAISecretStoreResult::Conflict);
	TestEqual(TEXT("Rejected stale write publishes no revision"), RejectedRevision, uint64{0});
	Replacement.Reset();
	TestEqual(TEXT("Stale delete revision fails closed"), Store->Delete(Context, Handle, FirstRevision, Error),
				   EUnrealAISecretStoreResult::Conflict);

	if (!TestEqual(TEXT("Replacement Keychain record loads"),
						Store->Load(Context, Handle, Loaded, LoadedRevision, Error),
						EUnrealAISecretStoreResult::Succeeded))
	{
		return false;
	}
	TestTrue(TEXT("Replacement returns guarded bytes"), Loaded.IsSet());
	TestEqual(TEXT("Replacement fixture length is preserved"), Loaded.Num(), 4);
	TestEqual(TEXT("Replacement revision round-trips"), LoadedRevision, ReplacementRevision);
	TestTrue(TEXT("Replacement fixture bytes round-trip exactly through the one-shot credential seam"),
				  VerifyFixtureSecret(MoveTemp(Loaded), 29, Clock));

	if (!TestEqual(TEXT("Current revision deletes the Keychain record"),
						Store->Delete(Context, Handle, ReplacementRevision, Error),
						EUnrealAISecretStoreResult::Succeeded))
	{
		return false;
	}
	TestEqual(TEXT("Deleted Keychain record is absent"), Store->Load(Context, Handle, Loaded, LoadedRevision, Error),
				   EUnrealAISecretStoreResult::NotFound);
	TestFalse(TEXT("Deleted Keychain load returns no secret"), Loaded.IsSet());
	TestEqual(TEXT("Deleted Keychain load returns revision zero"), LoadedRevision, uint64{0});

	FUnrealAICancellationSource CancelledSource;
	FUnrealAISecretStoreOperationContext CancelledContext;
	TestTrue(TEXT("Cancelled-operation context constructs before cancellation"),
				  FUnrealAISecretStoreOperationContext::TryCreate(Clock, 5.0, CancelledSource.GetToken(),
																  CancelledContext, ContextError));
	CancelledSource.Cancel(EUnrealAICancellationReason::Requested);
	TestEqual(TEXT("Cancelled Keychain operation fails before platform access"),
				   Store->Load(CancelledContext, Handle, Loaded, LoadedRevision, Error),
				   EUnrealAISecretStoreResult::Cancelled);
	TestEqual(TEXT("Cancelled Keychain operation has a typed public error"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreCancelled);

	FUnrealAICancellationSource TimedSource;
	FUnrealAISecretStoreOperationContext TimedContext;
	TestTrue(TEXT("Timed-operation context constructs"),
				  FUnrealAISecretStoreOperationContext::TryCreate(Clock, 1.0, TimedSource.GetToken(), TimedContext,
																  ContextError));
	MutableClock->Advance(FTimespan::FromSeconds(2.0));
	TestEqual(TEXT("Expired Keychain operation fails before platform access"),
				   Store->Load(TimedContext, Handle, Loaded, LoadedRevision, Error),
				   EUnrealAISecretStoreResult::TimedOut);
	TestEqual(TEXT("Expired Keychain operation has a typed public error"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut);

	FUnrealAISecretHandle WrongStoreHandle = Handle;
	WrongStoreHandle.StoreName = TEXT("tests.other_store");
	TestEqual(TEXT("Wrong-store handle fails before platform access"),
				   Store->Load(Context, WrongStoreHandle, Loaded, LoadedRevision, Error),
				   EUnrealAISecretStoreResult::Failed);
	TestEqual(TEXT("Wrong-store handle has a typed public error"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretHandleStoreMismatch);
#else
	TestFalse(TEXT("Non-macOS adapter is not persistent"), Capabilities.bPersistent);
	TestFalse(TEXT("Non-macOS adapter has no compare-and-swap support"), Capabilities.bAtomicCompareAndSwap);
	TestFalse(TEXT("Non-macOS adapter is unavailable in Shipping"), Capabilities.bAvailableInShipping);

	FUnrealAIProviderAccessError Error;
	FUnrealAISecretValue Loaded;
	uint64 Revision = 0;
	TestEqual(TEXT("Unsupported adapter rejects load deterministically"),
				   Store->Load(Context, Handle, Loaded, Revision, Error), EUnrealAISecretStoreResult::NotSupported);
	TestEqual(TEXT("Unsupported load has a closed error code"), Error.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
	FUnrealAISecretValue Fixture;
	TestTrue(TEXT("Unsupported-store fixture is guarded"), MakeFixtureSecret(41, Fixture));
	TestEqual(TEXT("Unsupported adapter rejects store deterministically"),
				   Store->Store(Context, Handle, Fixture, 0, Revision, Error),
				   EUnrealAISecretStoreResult::NotSupported);
	TestEqual(TEXT("Unsupported adapter rejects delete deterministically"), Store->Delete(Context, Handle, 1, Error),
				   EUnrealAISecretStoreResult::NotSupported);
	Fixture.Reset();
#endif
	return true;
}

#endif // WITH_AUTOMATION_TESTS
