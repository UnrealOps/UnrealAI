// Copyright EngineWorks. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"
#include "Misc/ScopeLock.h"

#include "Auth/UnrealAIApiKeyProvisioner.h"
#include "Auth/UnrealAIMemorySecretStore.h"

namespace
{
constexpr TCHAR ProvisionStoreName[] = TEXT("tests.provision.store");
const FName ProvisionProfile(TEXT("tests.provision.profile"));
const FName ProvisionAlias(TEXT("tests.provision.connection"));

FUnrealAIAccessAccountId MakeProvisionAccount()
{
	FUnrealAIAccessAccountId Account;
	Account.Value = FGuid(0x16500001, 0x16500002, 0x16500003, 0x16500004);
	return Account;
}

FUnrealAISecretHandle MakeProvisionHandle()
{
	FUnrealAISecretHandle Handle;
	Handle.StoreName = ProvisionStoreName;
	Handle.Value = FGuid(0x16600001, 0x16600002, 0x16600003, 0x16600004);
	return Handle;
}

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe>
MakeProvisionConnections(const FUnrealAIAccessAccountId &Account, FString &OutError)
{
	FUnrealAIEndpointOrigin Origin;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://provider.example.com"), false, Origin, OutError);
	FUnrealAIEndpointProfileRegistry Profiles;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Endpoint;
	Profiles.RegisterCustomApiEndpoint(TEXT("tests.provision.endpoint"),
											TEXT("tests.provision.provider"), Origin,
												 TEXT("https://provider.example.com/v1/model"), 1, Endpoint, OutError);
	const TSharedRef<FUnrealAIConnectionRegistry, ESPMode::ThreadSafe> Registry =
		MakeShared<FUnrealAIConnectionRegistry, ESPMode::ThreadSafe>(Profiles.CreateSnapshot());
	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionRevision = 1;
	Connection.ConnectionAlias = ProvisionAlias;
	Connection.EndpointProfileId = TEXT("tests.provision.endpoint");
	Connection.CredentialDestination.ModelProviderName = TEXT("tests.provision.provider");
	Connection.CredentialDestination.AccountAuthProviderName = TEXT("tests.platform.key");
	Connection.CredentialDestination.AuthProfileId = ProvisionProfile;
	Connection.CredentialDestination.AccountId = Account;
	Connection.CredentialDestination.TenantRealm = TEXT("tests.local");
	Connection.CredentialDestination.BillingPrincipalId.Value = FGuid(0x16610001, 0x16610002, 0x16610003, 0x16610004);
	Connection.CredentialDestination.PayerHandle = TEXT("tests.provision.payer");
	Connection.CredentialDestination.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Connection.CredentialDestination.BillingMode = EUnrealAIBillingMode::ApiMetered;
	Connection.CredentialDestination.EndpointOrigin = Origin;
	Connection.CredentialDestination.Audience = TEXT("https://provider.example.com/v1/model");
	Connection.CredentialDestination.ConnectionRevision = 1;
	Connection.CredentialDestination.EndpointPolicyRevision = 1;
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Registered;
	Registry->Register(Connection, Registered, OutError);
	return Registry->CreateSnapshot();
}

class FProvisionSink final : public IUnrealAIApiKeyProvisionSink
{
  public:
	FProvisionSink() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
	~FProvisionSink() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	void EnqueueApiKeyProvisionResult(FUnrealAIApiKeyProvisionResult &&Result) override
	{
		{
			FScopeLock Lock(&Mutex);
			Last = MakeUnique<FUnrealAIApiKeyProvisionResult>(MoveTemp(Result));
			++Count;
		}
		Event->Trigger();
	}

	bool WaitAndTake(FUnrealAIApiKeyProvisionResult &OutResult)
	{
		if (!Event->Wait(3000))
		{
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (!Last.IsValid())
		{
			return false;
		}
		OutResult = MoveTemp(*Last);
		Last.Reset();
		return true;
	}

	int32 GetCount() const
	{
		FScopeLock Lock(&Mutex);
		return Count;
	}

  private:
	mutable FCriticalSection Mutex;
	FEvent *Event = nullptr;
	TUniquePtr<FUnrealAIApiKeyProvisionResult> Last;
	int32 Count = 0;
};

class FProvisionCredentialSink final : public IUnrealAICredentialResultSink
{
  public:
	FProvisionCredentialSink() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
	~FProvisionCredentialSink() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	void EnqueueCredentialResult(FUnrealAICredentialResult &&Result) override
	{
		{
			FScopeLock Lock(&Mutex);
			Last = MakeUnique<FUnrealAICredentialResult>(MoveTemp(Result));
		}
		Event->Trigger();
	}

	bool WaitAndTake(FUnrealAICredentialResult &OutResult)
	{
		if (!Event->Wait(3000))
		{
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (!Last.IsValid())
		{
			return false;
		}
		OutResult = MoveTemp(*Last);
		Last.Reset();
		return true;
	}

  private:
	FCriticalSection Mutex;
	FEvent *Event = nullptr;
	TUniquePtr<FUnrealAICredentialResult> Last;
};

/** Models a platform backend that commits and only then observes the logical terminal. */
class FProvisionPostCommitTerminalStore final : public IUnrealAISecretStore
{
  public:
	explicit FProvisionPostCommitTerminalStore(TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> InInner)
		: Inner(MoveTemp(InInner))
	{
	}

	FName GetStoreName() const override
	{
		return Inner->GetStoreName();
	}

	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override
	{
		return Inner->DescribeCapabilities();
	}

	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &Context,
									const FUnrealAISecretHandle &Handle, FUnrealAISecretValue &OutValue,
									uint64 &OutRevision, FUnrealAIProviderAccessError &OutError) override
	{
		return Inner->Load(Context, Handle, OutValue, OutRevision, OutError);
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &Context,
									 const FUnrealAISecretHandle &Handle, const FUnrealAISecretValue &Value,
									 const uint64 ExpectedRevision, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		const EUnrealAISecretStoreResult Result =
			Inner->Store(Context, Handle, Value, ExpectedRevision, OutNewRevision, OutError);
		EUnrealAISecretStoreResult TerminalResult = EUnrealAISecretStoreResult::Succeeded;
		TFunction<void()> PostCommit;
		if (Result != EUnrealAISecretStoreResult::Succeeded || !TakeStoreHook(TerminalResult, PostCommit))
		{
			return Result;
		}
		++PostCommitStoreTerminals;
		PostCommit();
		OutNewRevision = 0;
		SetTerminalError(TerminalResult, OutError);
		return TerminalResult;
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &Context,
									  const FUnrealAISecretHandle &Handle, const uint64 ExpectedRevision,
									  FUnrealAIProviderAccessError &OutError) override
	{
		const EUnrealAISecretStoreResult Result = Inner->Delete(Context, Handle, ExpectedRevision, OutError);
		EUnrealAISecretStoreResult TerminalResult = EUnrealAISecretStoreResult::Succeeded;
		TFunction<void()> PostCommit;
		if (Result != EUnrealAISecretStoreResult::Succeeded || !TakeDeleteHook(TerminalResult, PostCommit))
		{
			return Result;
		}
		++PostCommitDeleteTerminals;
		PostCommit();
		SetTerminalError(TerminalResult, OutError);
		return TerminalResult;
	}

	void ArmNextStoreTerminal(const EUnrealAISecretStoreResult TerminalResult, TFunction<void()> PostCommit)
	{
		FScopeLock Lock(&HookMutex);
		StoreTerminalResult = TerminalResult;
		StorePostCommit = MoveTemp(PostCommit);
	}

	void ArmNextDeleteTerminal(const EUnrealAISecretStoreResult TerminalResult, TFunction<void()> PostCommit)
	{
		FScopeLock Lock(&HookMutex);
		DeleteTerminalResult = TerminalResult;
		DeletePostCommit = MoveTemp(PostCommit);
	}

	int32 GetPostCommitStoreTerminalCount() const
	{
		return PostCommitStoreTerminals.Load();
	}

	int32 GetPostCommitDeleteTerminalCount() const
	{
		return PostCommitDeleteTerminals.Load();
	}

  private:
	bool TakeStoreHook(EUnrealAISecretStoreResult &OutResult, TFunction<void()> &OutPostCommit)
	{
		FScopeLock Lock(&HookMutex);
		if (!StorePostCommit)
		{
			return false;
		}
		OutResult = StoreTerminalResult;
		OutPostCommit = MoveTemp(StorePostCommit);
		return true;
	}

	bool TakeDeleteHook(EUnrealAISecretStoreResult &OutResult, TFunction<void()> &OutPostCommit)
	{
		FScopeLock Lock(&HookMutex);
		if (!DeletePostCommit)
		{
			return false;
		}
		OutResult = DeleteTerminalResult;
		OutPostCommit = MoveTemp(DeletePostCommit);
		return true;
	}

	static void SetTerminalError(const EUnrealAISecretStoreResult Result, FUnrealAIProviderAccessError &OutError)
	{
		OutError = {};
		if (Result == EUnrealAISecretStoreResult::Cancelled)
		{
			OutError.Category = EUnrealAIErrorCategory::Cancelled;
			OutError.Code = EUnrealAIProviderAccessErrorCode::SecretStoreCancelled;
		}
		else if (Result == EUnrealAISecretStoreResult::TimedOut)
		{
			OutError.Category = EUnrealAIErrorCategory::Timeout;
			OutError.Code = EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut;
			OutError.bRetryable = true;
		}
	}

	TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Inner;
	FCriticalSection HookMutex;
	EUnrealAISecretStoreResult StoreTerminalResult = EUnrealAISecretStoreResult::Succeeded;
	EUnrealAISecretStoreResult DeleteTerminalResult = EUnrealAISecretStoreResult::Succeeded;
	TFunction<void()> StorePostCommit;
	TFunction<void()> DeletePostCommit;
	TAtomic<int32> PostCommitStoreTerminals{0};
	TAtomic<int32> PostCommitDeleteTerminals{0};
};

FUnrealAICredentialRequest MakeProvisionCredentialRequest(const uint32 Seed)
{
	FUnrealAICredentialRequest Request;
	Request.RequestId.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	Request.ConnectionAlias = ProvisionAlias;
	Request.TimeoutSeconds = 5.0f;
	return Request;
}

bool WaitForProvisionCondition(TFunctionRef<bool()> Predicate)
{
	const double Deadline = FPlatformTime::Seconds() + 3.0;
	while (!Predicate() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return Predicate();
}

bool ReadProvisionedSecret(IUnrealAISecretStore &Store, TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
						   const FUnrealAISecretHandle &Handle, int32 &OutByteCount)
{
	OutByteCount = 0;
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	FString Error;
	if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context, Error))
	{
		return false;
	}
	FUnrealAISecretValue Secret;
	uint64 Revision = 0;
	FUnrealAIProviderAccessError StoreError;
	if (Store.Load(Context, Handle, Secret, Revision, StoreError) != EUnrealAISecretStoreResult::Succeeded ||
		Revision == 0 || !Secret.IsSet())
	{
		return false;
	}
	OutByteCount = Secret.Num();
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyProvisionerStoreDeleteTest,
								 "UnrealAI.Auth.ApiKeyProvisioner.StoreDeleteTerminalOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyProvisionerStoreDeleteTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeProvisionAccount();
	const FUnrealAISecretHandle Handle = MakeProvisionHandle();
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeProvisionConnections(Account, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 8, 10), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Store =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(ProvisionStoreName, 8);
	const TSharedRef<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> Broker =
		MakeShared<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe>(Connections, Store, Clock);
	FUnrealAIApiKeyCredentialBinding Binding;
	Binding.ConnectionAlias = ProvisionAlias;
	Binding.AuthProfileId = ProvisionProfile;
	Binding.AccountId = Account;
	Binding.SecretHandle = Handle;
	TestTrue(TEXT("Provision test broker binding registers"), Broker->RegisterBinding(Binding, Error));

	FUnrealAIApiKeyProvisionerConfig Config;
	Config.AuthProfileId = ProvisionProfile;
	Config.AccountId = Account;
	Config.SecretHandle = Handle;
	TSharedPtr<FUnrealAIApiKeyProvisioner, ESPMode::ThreadSafe> Provisioner;
	TestTrue(TEXT("Provisioner accepts exact secure-store binding"),
				  FUnrealAIApiKeyProvisioner::TryCreate(Config, Store, Clock, Broker, Provisioner, Error));
	if (!Provisioner.IsValid())
	{
		return false;
	}

	TArray<uint8> Bytes{1, 3, 3, 7};
	FUnrealAISecretValue Secret;
	TestTrue(TEXT("Provision fixture creates move-only secret"),
				  FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), Secret, Error));
	FUnrealAICancellationSource StoreCancellation;
	const TSharedRef<FProvisionSink, ESPMode::ThreadSafe> StoreSink = MakeShared<FProvisionSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> StoreHandle;
	FUnrealAIProviderAccessError StartError;
	FUnrealAIRequestId StoreRequest;
	StoreRequest.Value = FGuid(0x16620001, 0x16620002, 0x16620003, 0x16620004);
	TestTrue(TEXT("Store operation starts"),
				  Provisioner->StartStore(StoreRequest, MoveTemp(Secret), 5.0f, StoreSink, StoreCancellation.GetToken(),
										  StoreHandle, StartError));
	FUnrealAIApiKeyProvisionResult StoreResult;
	TestTrue(TEXT("Store operation publishes"), StoreSink->WaitAndTake(StoreResult));
	TestEqual(TEXT("Store terminal kind"), StoreResult.Kind, EUnrealAIApiKeyProvisionKind::Stored);
	TestEqual(TEXT("Store publishes exactly once"), StoreSink->GetCount(), 1);
	int32 StoredByteCount = 0;
	TestTrue(TEXT("Opaque secure store contains provisioned bytes"),
				  ReadProvisionedSecret(*Store, Clock, Handle, StoredByteCount));
	TestEqual(TEXT("Provisioned byte count"), StoredByteCount, 4);

	FUnrealAICancellationSource DeleteCancellation;
	const TSharedRef<FProvisionSink, ESPMode::ThreadSafe> DeleteSink =
		MakeShared<FProvisionSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> DeleteHandle;
	FUnrealAIRequestId DeleteRequest;
	DeleteRequest.Value = FGuid(0x16630001, 0x16630002, 0x16630003, 0x16630004);
	TestTrue(TEXT("Delete operation starts"),
				  Provisioner->StartDelete(DeleteRequest, 5.0f, DeleteSink, DeleteCancellation.GetToken(), DeleteHandle,
										   StartError));
	FUnrealAIApiKeyProvisionResult DeleteResult;
	TestTrue(TEXT("Delete operation publishes"), DeleteSink->WaitAndTake(DeleteResult));
	TestEqual(TEXT("Delete terminal kind"), DeleteResult.Kind, EUnrealAIApiKeyProvisionKind::Deleted);
	TestEqual(TEXT("Delete publishes exactly once"), DeleteSink->GetCount(), 1);
	StoredByteCount = 0;
	TestFalse(TEXT("Deleted secret is absent"), ReadProvisionedSecret(*Store, Clock, Handle, StoredByteCount));

	Provisioner->BeginShutdown();
	Broker->BeginShutdown();
	TestTrue(TEXT("Provisioner shutdown is observable"), Provisioner->IsShutdown());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyProvisionerPostCommitTerminalInvalidationTest,
								 "UnrealAI.Auth.ApiKeyProvisioner.PostCommitCancelTimeoutInvalidateLeases",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyProvisionerPostCommitTerminalInvalidationTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeProvisionAccount();
	const FUnrealAISecretHandle Handle = MakeProvisionHandle();
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeProvisionConnections(Account, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 8, 10), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(ProvisionStoreName, 8);
	const TSharedRef<FProvisionPostCommitTerminalStore, ESPMode::ThreadSafe> PostCommitStore =
		MakeShared<FProvisionPostCommitTerminalStore, ESPMode::ThreadSafe>(Memory);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = PostCommitStore;
	const TSharedRef<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe> Broker =
		MakeShared<FUnrealAIApiKeyCredentialBroker, ESPMode::ThreadSafe>(Connections, Store, Clock);
	FUnrealAIApiKeyCredentialBinding Binding;
	Binding.ConnectionAlias = ProvisionAlias;
	Binding.AuthProfileId = ProvisionProfile;
	Binding.AccountId = Account;
	Binding.SecretHandle = Handle;
	if (!TestTrue(TEXT("Post-commit test broker binding registers"), Broker->RegisterBinding(Binding, Error)))
	{
		return false;
	}

	FUnrealAIApiKeyProvisionerConfig Config;
	Config.AuthProfileId = ProvisionProfile;
	Config.AccountId = Account;
	Config.SecretHandle = Handle;
	TSharedPtr<FUnrealAIApiKeyProvisioner, ESPMode::ThreadSafe> Provisioner;
	if (!TestTrue(TEXT("Post-commit test provisioner constructs"),
					   FUnrealAIApiKeyProvisioner::TryCreate(Config, Store, Clock, Broker, Provisioner, Error)) ||
				  !Provisioner.IsValid())
	{
		return false;
	}

	TArray<uint8> InitialBytes{1, 2, 3, 4};
	FUnrealAISecretValue InitialSecret;
	if (!TestTrue(TEXT("Initial API-key fixture constructs"),
					   FUnrealAISecretValue::TryCreate(MoveTemp(InitialBytes), InitialSecret, Error)))
	{
		return false;
	}
	FUnrealAICancellationSource InitialCancellation;
	const TSharedRef<FProvisionSink, ESPMode::ThreadSafe> InitialSink =
		MakeShared<FProvisionSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> InitialHandle;
	FUnrealAIProviderAccessError StartError;
	FUnrealAIRequestId InitialRequest;
	InitialRequest.Value = FGuid(0x16640001, 0x16640002, 0x16640003, 0x16640004);
	if (!TestTrue(TEXT("Initial API key stores"),
					   Provisioner->StartStore(InitialRequest, MoveTemp(InitialSecret), 5.0f, InitialSink,
											   InitialCancellation.GetToken(), InitialHandle, StartError)))
	{
		return false;
	}
	FUnrealAIApiKeyProvisionResult InitialResult;
	if (!TestTrue(TEXT("Initial API-key store publishes"), InitialSink->WaitAndTake(InitialResult)))
	{
		return false;
	}
	TestEqual(TEXT("Initial API-key store succeeds"), InitialResult.Kind, EUnrealAIApiKeyProvisionKind::Stored);

	FUnrealAICancellationSource OldLeaseCancellation;
	const TSharedRef<FProvisionCredentialSink, ESPMode::ThreadSafe> OldLeaseSink =
		MakeShared<FProvisionCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> OldLeaseHandle;
	if (!TestTrue(TEXT("Pre-rotation credential lease resolves"),
					   Broker->StartResolve(MakeProvisionCredentialRequest(0x16650001), OldLeaseSink,
											OldLeaseCancellation.GetToken(), OldLeaseHandle, StartError)))
	{
		return false;
	}
	FUnrealAICredentialResult OldLeaseResult;
	if (!TestTrue(TEXT("Pre-rotation credential lease publishes"), OldLeaseSink->WaitAndTake(OldLeaseResult)))
	{
		return false;
	}
	TestEqual(TEXT("Pre-rotation credential lease succeeds"), OldLeaseResult.Kind,
				   EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("Pre-rotation credential context is usable"), OldLeaseResult.AccessContext.IsValid());
	TestEqual(TEXT("One pre-rotation credential lease is active"), Broker->GetActiveLeaseCount(), 1);

	FUnrealAICancellationSource RotationCancellation;
	PostCommitStore->ArmNextStoreTerminal(EUnrealAISecretStoreResult::Cancelled, [RotationCancellation]()
										  { RotationCancellation.Cancel(EUnrealAICancellationReason::Requested); });
	TArray<uint8> RotatedBytes{5, 6, 7, 8, 9};
	FUnrealAISecretValue RotatedSecret;
	if (!TestTrue(TEXT("Rotated API-key fixture constructs"),
					   FUnrealAISecretValue::TryCreate(MoveTemp(RotatedBytes), RotatedSecret, Error)))
	{
		return false;
	}
	const TSharedRef<FProvisionSink, ESPMode::ThreadSafe> RotationSink =
		MakeShared<FProvisionSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> RotationHandle;
	FUnrealAIRequestId RotationRequest;
	RotationRequest.Value = FGuid(0x16660001, 0x16660002, 0x16660003, 0x16660004);
	if (!TestTrue(TEXT("Post-commit cancellation rotation starts"),
					   Provisioner->StartStore(RotationRequest, MoveTemp(RotatedSecret), 5.0f, RotationSink,
											   RotationCancellation.GetToken(), RotationHandle, StartError)))
	{
		return false;
	}
	FUnrealAIApiKeyProvisionResult RotationResult;
	if (!TestTrue(TEXT("Post-commit cancellation rotation publishes"), RotationSink->WaitAndTake(RotationResult)))
	{
		return false;
	}
	TestEqual(TEXT("Committed rotation remains logically cancelled"), RotationResult.Kind,
				   EUnrealAIApiKeyProvisionKind::Cancelled);
	TestEqual(TEXT("One physical store committed before cancellation"),
				   PostCommitStore->GetPostCommitStoreTerminalCount(), 1);
	TestTrue(TEXT("Committed-then-cancelled rotation revokes the prior lease"),
				  WaitForProvisionCondition([Broker]() { return Broker->GetActiveLeaseCount() == 0; }));
	if (OldLeaseResult.AccessContext.IsValid())
	{
		TestFalse(TEXT("Prior credential context is stale after ambiguous rotation"),
					   OldLeaseResult.AccessContext->IsValid());
	}
	int32 StoredByteCount = 0;
	TestTrue(TEXT("Cancelled logical rotation physically stored the replacement"),
				  ReadProvisionedSecret(*Memory, Clock, Handle, StoredByteCount));
	TestEqual(TEXT("Physically rotated API-key byte count"), StoredByteCount, 5);
	TestEqual(TEXT("Post-commit cancellation publishes exactly once"), RotationSink->GetCount(), 1);

	FUnrealAICancellationSource RotatedLeaseCancellation;
	const TSharedRef<FProvisionCredentialSink, ESPMode::ThreadSafe> RotatedLeaseSink =
		MakeShared<FProvisionCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RotatedLeaseHandle;
	if (!TestTrue(TEXT("Post-rotation credential lease resolves"),
					   Broker->StartResolve(MakeProvisionCredentialRequest(0x16670001), RotatedLeaseSink,
											RotatedLeaseCancellation.GetToken(), RotatedLeaseHandle, StartError)))
	{
		return false;
	}
	FUnrealAICredentialResult RotatedLeaseResult;
	if (!TestTrue(TEXT("Post-rotation credential lease publishes"), RotatedLeaseSink->WaitAndTake(RotatedLeaseResult)))
	{
		return false;
	}
	TestEqual(TEXT("Post-rotation credential lease succeeds"), RotatedLeaseResult.Kind,
				   EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("Post-rotation credential context is usable"), RotatedLeaseResult.AccessContext.IsValid());
	TestEqual(TEXT("One post-rotation credential lease is active"), Broker->GetActiveLeaseCount(), 1);

	FUnrealAICancellationSource DeleteCancellation;
	PostCommitStore->ArmNextDeleteTerminal(EUnrealAISecretStoreResult::TimedOut,
										   [MutableClock]() { MutableClock->Advance(FTimespan::FromSeconds(6.0)); });
	const TSharedRef<FProvisionSink, ESPMode::ThreadSafe> DeleteSink =
		MakeShared<FProvisionSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAIApiKeyProvisionHandle, ESPMode::ThreadSafe> DeleteHandle;
	FUnrealAIRequestId DeleteRequest;
	DeleteRequest.Value = FGuid(0x16680001, 0x16680002, 0x16680003, 0x16680004);
	if (!TestTrue(TEXT("Post-commit timeout delete starts"),
					   Provisioner->StartDelete(DeleteRequest, 5.0f, DeleteSink, DeleteCancellation.GetToken(),
												DeleteHandle, StartError)))
	{
		return false;
	}
	FUnrealAIApiKeyProvisionResult DeleteResult;
	if (!TestTrue(TEXT("Post-commit timeout delete publishes"), DeleteSink->WaitAndTake(DeleteResult)))
	{
		return false;
	}
	TestEqual(TEXT("Committed deletion remains logically timed out"), DeleteResult.Kind,
				   EUnrealAIApiKeyProvisionKind::TimedOut);
	TestEqual(TEXT("One physical delete committed before timeout"), PostCommitStore->GetPostCommitDeleteTerminalCount(),
				   1);
	TestTrue(TEXT("Committed-then-timed-out deletion revokes the prior lease"),
				  WaitForProvisionCondition([Broker]() { return Broker->GetActiveLeaseCount() == 0; }));
	if (RotatedLeaseResult.AccessContext.IsValid())
	{
		TestFalse(TEXT("Prior credential context is stale after ambiguous deletion"),
					   RotatedLeaseResult.AccessContext->IsValid());
	}
	StoredByteCount = 0;
	TestFalse(TEXT("Timed-out logical deletion physically removed the API key"),
				   ReadProvisionedSecret(*Memory, Clock, Handle, StoredByteCount));
	TestEqual(TEXT("Post-commit timeout publishes exactly once"), DeleteSink->GetCount(), 1);

	Provisioner->BeginShutdown();
	Broker->BeginShutdown();
	return true;
}
