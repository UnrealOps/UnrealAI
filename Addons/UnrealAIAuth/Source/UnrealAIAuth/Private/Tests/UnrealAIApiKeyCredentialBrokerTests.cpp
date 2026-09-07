// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Testing/UnrealAITestClock.h"
#include "Misc/ScopeLock.h"

#include "Async/ParallelFor.h"
#include "Auth/UnrealAIApiKeyCredentialBroker.h"
#include "Auth/UnrealAIMemorySecretStore.h"

namespace
{
constexpr TCHAR TestStoreName[] = TEXT("tests.api_key_store");
constexpr TCHAR TestAuthProfile[] = TEXT("tests.api_key_profile");

bool MakeBrokerTestSecret(const TArray<uint8> &Fixture, FUnrealAISecretValue &OutSecret, FString &OutError)
{
	TArray<uint8> Bytes = Fixture;
	return FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutSecret, OutError);
}

FUnrealAIAccessAccountId MakeBrokerTestAccount(const uint32 Seed)
{
	FUnrealAIAccessAccountId Account;
	Account.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	return Account;
}

FUnrealAISecretHandle MakeBrokerTestHandle(const uint32 Seed)
{
	FUnrealAISecretHandle Handle;
	Handle.StoreName = TestStoreName;
	Handle.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	return Handle;
}

FUnrealAIConnectionDescriptor MakeBrokerTestApiKeyConnection(const FName Alias, const FUnrealAIAccessAccountId &Account,
															 FString &OutError,
															 const FName AuthProfileId = TestAuthProfile)
{
	FUnrealAIConnectionDescriptor Connection;
	Connection.ConnectionRevision = 7;
	Connection.ConnectionAlias = Alias;
	Connection.EndpointProfileId = TEXT("tests.api_key_endpoint");
	FUnrealAICredentialDestination &Destination = Connection.CredentialDestination;
	Destination.ModelProviderName = TEXT("openai.compatible");
	Destination.AccountAuthProviderName = TEXT("project.secrets");
	Destination.AuthProfileId = AuthProfileId;
	Destination.AccountId = Account;
	Destination.TenantRealm = TEXT("tests.tenant");
	Destination.BillingPrincipalId.Value = FGuid(100, 101, 102, 103);
	Destination.PayerHandle = TEXT("tests.api_payer");
	Destination.AuthScheme = EUnrealAIAuthScheme::ApiKey;
	Destination.BillingMode = EUnrealAIBillingMode::ApiMetered;
	Destination.Audience = TEXT("openai-compatible-api");
	Destination.ConnectionRevision = Connection.ConnectionRevision;
	Destination.EndpointPolicyRevision = 11;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://api.example.com"), false, Destination.EndpointOrigin, OutError);
	return Connection;
}

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe>
MakeBrokerTestConnectionSnapshot(const TArray<FUnrealAIConnectionDescriptor> &Connections, FString &OutError)
{
	FUnrealAIEndpointProfileRegistry Profiles;
	FUnrealAIEndpointOrigin Origin;
	FUnrealAIEndpointOrigin::TryParse(TEXT("https://api.example.com"), false, Origin, OutError);
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Profile;
	Profiles.RegisterCustomApiEndpoint(TEXT("tests.api_key_endpoint"), TEXT("openai.compatible"), Origin,
																			TEXT("openai-compatible-api"), 11, Profile,
																				 OutError);
	const TSharedRef<FUnrealAIConnectionRegistry, ESPMode::ThreadSafe> Registry =
		MakeShared<FUnrealAIConnectionRegistry, ESPMode::ThreadSafe>(Profiles.CreateSnapshot());
	for (const FUnrealAIConnectionDescriptor &Connection : Connections)
	{
		TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Registered;
		Registry->Register(Connection, Registered, OutError);
	}
	return Registry->CreateSnapshot();
}

TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe>
MakeBrokerDistinctEndpointSnapshot(const FUnrealAIConnectionDescriptor &Primary,
								   const FUnrealAIConnectionDescriptor &Secondary, FString &OutError)
{
	FUnrealAIEndpointProfileRegistry Profiles;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Profile;
	Profiles.RegisterCustomApiEndpoint(Primary.EndpointProfileId, Primary.CredentialDestination.ModelProviderName,
									   Primary.CredentialDestination.EndpointOrigin,
									   Primary.CredentialDestination.Audience,
									   Primary.CredentialDestination.EndpointPolicyRevision, Profile, OutError);
	Profiles.RegisterCustomApiEndpoint(Secondary.EndpointProfileId, Secondary.CredentialDestination.ModelProviderName,
									   Secondary.CredentialDestination.EndpointOrigin,
									   Secondary.CredentialDestination.Audience,
									   Secondary.CredentialDestination.EndpointPolicyRevision, Profile, OutError);
	const TSharedRef<FUnrealAIConnectionRegistry, ESPMode::ThreadSafe> Registry =
		MakeShared<FUnrealAIConnectionRegistry, ESPMode::ThreadSafe>(Profiles.CreateSnapshot());
	for (const FUnrealAIConnectionDescriptor *Connection : {&Primary, &Secondary})
	{
		TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> Registered;
		Registry->Register(*Connection, Registered, OutError);
	}
	return Registry->CreateSnapshot();
}

FUnrealAICredentialRequest MakeBrokerTestRequest(const uint32 Seed, const FName Alias,
												 const float TimeoutSeconds = 30.0f)
{
	FUnrealAICredentialRequest Request;
	Request.RequestId.Value = FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	Request.ConnectionAlias = Alias;
	Request.TimeoutSeconds = TimeoutSeconds;
	return Request;
}

bool WaitForBrokerTestCondition(TFunctionRef<bool()> Predicate, const double TimeoutSeconds = 2.0)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!Predicate() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return Predicate();
}

class FBrokerTestRecordingCredentialSink final : public IUnrealAICredentialResultSink
{
  public:
	FBrokerTestRecordingCredentialSink() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
	~FBrokerTestRecordingCredentialSink() override
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);
	}

	void EnqueueCredentialResult(FUnrealAICredentialResult &&Result) override
	{
		{
			FScopeLock Lock(&Mutex);
			++Count;
			LastResult = MakeUnique<FUnrealAICredentialResult>(MoveTemp(Result));
		}
		Event->Trigger();
	}

	bool WaitAndTake(FUnrealAICredentialResult &OutResult, const uint32 TimeoutMilliseconds = 2000)
	{
		if (!Event->Wait(TimeoutMilliseconds))
		{
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (!LastResult.IsValid())
		{
			return false;
		}
		OutResult = MoveTemp(*LastResult);
		LastResult.Reset();
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
	TUniquePtr<FUnrealAICredentialResult> LastResult;
	int32 Count = 0;
};

class FBrokerTestCredentialApplicator final : public IUnrealAICredentialApplicator
{
  public:
	explicit FBrokerTestCredentialApplicator(const FUnrealAICredentialDestination &InDestination)
		: Destination(InDestination)
	{
	}

	const FUnrealAICredentialDestination &GetActualDestination() const override
	{
		return Destination;
	}

	bool ApplyCredentialAndDispatch(const EUnrealAIAuthScheme Scheme, const TConstArrayView<uint8> Secret) override
	{
		ObservedScheme = Scheme;
		ObservedBytes = Secret.Num();
		ObservedSum = 0;
		for (const uint8 Byte : Secret)
		{
			ObservedSum += Byte;
		}
		return true;
	}

	bool DispatchWithoutCredential() override
	{
		return false;
	}

	FUnrealAICredentialDestination Destination;
	EUnrealAIAuthScheme ObservedScheme = EUnrealAIAuthScheme::Invalid;
	int32 ObservedBytes = 0;
	uint32 ObservedSum = 0;
};

class FBrokerTestThreadRecordingSecretStore final : public IUnrealAISecretStore
{
  public:
	explicit FBrokerTestThreadRecordingSecretStore(TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> InInner)
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
		++LoadCalls;
		if (IsInGameThread())
		{
			bObservedGameThread.Store(true);
		}
		return Inner->Load(Context, Handle, OutValue, OutRevision, OutError);
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &Context,
									 const FUnrealAISecretHandle &Handle, const FUnrealAISecretValue &Value,
									 const uint64 ExpectedRevision, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		return Inner->Store(Context, Handle, Value, ExpectedRevision, OutNewRevision, OutError);
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &Context,
									  const FUnrealAISecretHandle &Handle, const uint64 ExpectedRevision,
									  FUnrealAIProviderAccessError &OutError) override
	{
		return Inner->Delete(Context, Handle, ExpectedRevision, OutError);
	}

	TAtomic<int32> LoadCalls{0};
	TAtomic<bool> bObservedGameThread{false};

  private:
	TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Inner;
};

class FBrokerTestBlockingSecretStoreControl final
{
  public:
	FBrokerTestBlockingSecretStoreControl()
		: Entered(FPlatformProcess::GetSynchEventFromPool(true)), Release(FPlatformProcess::GetSynchEventFromPool(true))
	{
	}
	~FBrokerTestBlockingSecretStoreControl()
	{
		Release->Trigger();
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		FPlatformProcess::ReturnSynchEventToPool(Release);
	}

	FEvent *Entered = nullptr;
	FEvent *Release = nullptr;
	TAtomic<int32> EnteredCount{0};
	TAtomic<bool> bObservedGameThread{false};
};

class FBrokerTestBlockingSecretStore final : public IUnrealAISecretStore
{
  public:
	explicit FBrokerTestBlockingSecretStore(
		TSharedRef<FBrokerTestBlockingSecretStoreControl, ESPMode::ThreadSafe> InControl)
		: Control(MoveTemp(InControl))
	{
	}

	FName GetStoreName() const override
	{
		return TestStoreName;
	}

	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override
	{
		FUnrealAISecretStoreCapabilities Capabilities;
		Capabilities.bAtomicCompareAndSwap = true;
		return Capabilities;
	}

	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									FUnrealAISecretValue &OutValue, uint64 &OutRevision,
									FUnrealAIProviderAccessError &OutError) override
	{
		OutValue.Reset();
		OutRevision = 0;
		OutError = FUnrealAIProviderAccessError{};
		if (IsInGameThread())
		{
			Control->bObservedGameThread.Store(true);
		}
		++Control->EnteredCount;
		Control->Entered->Trigger();
		Control->Release->Wait(5000);
		FString Error;
		TArray<uint8> Bytes{9, 8, 7, 6};
		if (!FUnrealAISecretValue::TryCreate(MoveTemp(Bytes), OutValue, Error))
		{
			OutError.Category = EUnrealAIErrorCategory::Internal;
			OutError.Code = EUnrealAIProviderAccessErrorCode::Internal;
			return EUnrealAISecretStoreResult::Failed;
		}
		OutRevision = 1;
		return EUnrealAISecretStoreResult::Succeeded;
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									 const FUnrealAISecretValue &, uint64, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		OutNewRevision = 0;
		OutError.Category = EUnrealAIErrorCategory::Internal;
		OutError.Code = EUnrealAIProviderAccessErrorCode::Internal;
		return EUnrealAISecretStoreResult::Failed;
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									  uint64, FUnrealAIProviderAccessError &OutError) override
	{
		OutError.Category = EUnrealAIErrorCategory::Internal;
		OutError.Code = EUnrealAIProviderAccessErrorCode::Internal;
		return EUnrealAISecretStoreResult::Failed;
	}

  private:
	TSharedRef<FBrokerTestBlockingSecretStoreControl, ESPMode::ThreadSafe> Control;
};

class FBrokerTestFaultSecretStore final : public IUnrealAISecretStore
{
  public:
	explicit FBrokerTestFaultSecretStore(const EUnrealAISecretStoreResult InResult) : Result(InResult) {}

	FName GetStoreName() const override
	{
		return TestStoreName;
	}

	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override
	{
		return FUnrealAISecretStoreCapabilities{};
	}

	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									FUnrealAISecretValue &OutValue, uint64 &OutRevision,
									FUnrealAIProviderAccessError &OutError) override
	{
		OutValue.Reset();
		OutRevision = 0;
		OutError = FUnrealAIProviderAccessError{};
		if (Result == EUnrealAISecretStoreResult::Failed)
		{
			OutError.Category = EUnrealAIErrorCategory::Internal;
			OutError.Code = EUnrealAIProviderAccessErrorCode::Internal;
		}
		return Result;
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									 const FUnrealAISecretValue &, uint64, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		OutNewRevision = 0;
		OutError.Category = EUnrealAIErrorCategory::Internal;
		OutError.Code = EUnrealAIProviderAccessErrorCode::Internal;
		return EUnrealAISecretStoreResult::Failed;
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									  uint64, FUnrealAIProviderAccessError &OutError) override
	{
		OutError.Category = EUnrealAIErrorCategory::Internal;
		OutError.Code = EUnrealAIProviderAccessErrorCode::Internal;
		return EUnrealAISecretStoreResult::Failed;
	}

  private:
	EUnrealAISecretStoreResult Result = EUnrealAISecretStoreResult::Failed;
};

bool PrepopulateBrokerTestSecret(FUnrealAIMemorySecretStore &Store,
								 TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock,
								 const FUnrealAISecretHandle &Handle, const TArray<uint8> &Bytes, FString &OutError)
{
	FUnrealAICancellationSource Cancellation;
	FUnrealAISecretStoreOperationContext Context;
	if (!FUnrealAISecretStoreOperationContext::TryCreate(Clock, 30.0, Cancellation.GetToken(), Context, OutError))
	{
		return false;
	}
	FUnrealAISecretValue Secret;
	if (!MakeBrokerTestSecret(Bytes, Secret, OutError))
	{
		return false;
	}
	uint64 Revision = 0;
	FUnrealAIProviderAccessError StoreError;
	return Store.Store(Context, Handle, Secret, 0, Revision, StoreError) == EUnrealAISecretStoreResult::Succeeded &&
		   Revision == 1;
}

bool RegisterBrokerTestBinding(FUnrealAIApiKeyCredentialBroker &Broker, const FName ConnectionAlias,
							   const FUnrealAIAccessAccountId &Account, const FUnrealAISecretHandle &Handle,
							   FString &OutError, const FName AuthProfileId = TestAuthProfile)
{
	FUnrealAIApiKeyCredentialBinding Binding;
	Binding.ConnectionAlias = ConnectionAlias;
	Binding.AuthProfileId = AuthProfileId;
	Binding.AccountId = Account;
	Binding.SecretHandle = Handle;
	return Broker.RegisterBinding(Binding, OutError);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyCredentialBrokerResolveTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.ResolveExactDestinationOffGameThread",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyCredentialBrokerResolveTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeBrokerTestAccount(1);
	const FUnrealAISecretHandle Handle = MakeBrokerTestHandle(10);
	const FUnrealAIConnectionDescriptor Connection =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_primary"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeBrokerTestConnectionSnapshot({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 10.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(TestStoreName, 4);
	TestTrue(TEXT("Secret fixture is stored before broker admission"),
				  PrepopulateBrokerTestSecret(*Memory, Clock, Handle, {1, 2, 3, 4}, Error));
	const TSharedRef<FBrokerTestThreadRecordingSecretStore, ESPMode::ThreadSafe> RecordingStore =
		MakeShared<FBrokerTestThreadRecordingSecretStore, ESPMode::ThreadSafe>(Memory);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = RecordingStore;
	FUnrealAIApiKeyCredentialBroker Broker(Connections, Store, Clock);
	TestTrue(TEXT("Non-secret profile/account binding registers"),
				  RegisterBrokerTestBinding(Broker, Connection.ConnectionAlias, Account, Handle, Error));

	FUnrealAICancellationSource Cancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
	FUnrealAIProviderAccessError AccessError;
	const FUnrealAICredentialRequest Request = MakeBrokerTestRequest(100, Connection.ConnectionAlias);
	TestTrue(TEXT("API-key resolution is admitted"),
				  Broker.StartResolve(Request, Sink, Cancellation.GetToken(), RequestHandle, AccessError));
	TestTrue(TEXT("Accepted resolution returns its exact request handle"), RequestHandle.IsValid());

	FUnrealAICredentialResult Result;
	TestTrue(TEXT("Resolution publishes a bounded terminal"), Sink->WaitAndTake(Result));
	TestEqual(TEXT("Resolution succeeds"), Result.Kind, EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("Success contains an opaque credential context"), Result.AccessContext.IsValid());
	TestFalse(TEXT("Blocking secret-store load never runs on the game thread"),
				   RecordingStore->bObservedGameThread.Load());
	TestEqual(TEXT("One physical load executes"), RecordingStore->LoadCalls.Load(), 1);

	FUnrealAICredentialDestination WrongDestination = Connection.CredentialDestination;
	WrongDestination.AccountId = MakeBrokerTestAccount(50);
	FBrokerTestCredentialApplicator WrongApplicator(WrongDestination);
	TestFalse(TEXT("Alias resolution cannot retarget the credential to another account"),
				   Result.AccessContext->TryDispatch(WrongApplicator, Error));
	TestEqual(TEXT("Rejected destination receives no credential bytes"), WrongApplicator.ObservedBytes, 0);
	TestTrue(TEXT("Pre-materialization rejection leaves the context bound to its frozen destination"),
				  Result.AccessContext->IsValid());
	FBrokerTestCredentialApplicator CorrectedApplicator(Connection.CredentialDestination);
	TestTrue(TEXT("The untouched context dispatches once to its frozen destination"),
				  Result.AccessContext->TryDispatch(CorrectedApplicator, Error));
	TestEqual(TEXT("Corrected dispatch preserves the API-key scheme"), CorrectedApplicator.ObservedScheme,
				   EUnrealAIAuthScheme::ApiKey);
	TestEqual(TEXT("Corrected dispatch preserves credential bytes"), CorrectedApplicator.ObservedBytes, 4);
	TestEqual(TEXT("Corrected dispatch preserves the credential byte sum"), CorrectedApplicator.ObservedSum,
				   uint32(10));
	TestFalse(TEXT("Credential presentation consumes the corrected one-shot context"), Result.AccessContext->IsValid());

	FUnrealAICancellationSource ExactCancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> ExactSink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> ExactHandle;
	TestTrue(TEXT("A fresh one-shot lease resolves for the exact destination"),
				  Broker.StartResolve(MakeBrokerTestRequest(150, Connection.ConnectionAlias), ExactSink,
									  ExactCancellation.GetToken(), ExactHandle, AccessError));
	FUnrealAICredentialResult ExactResult;
	TestTrue(TEXT("Fresh exact-destination lease publishes success"), ExactSink->WaitAndTake(ExactResult));
	TestEqual(TEXT("Fresh exact-destination resolution succeeds"), ExactResult.Kind,
				   EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("Fresh exact-destination result contains an opaque context"), ExactResult.AccessContext.IsValid());
	FBrokerTestCredentialApplicator ExactApplicator(Connection.CredentialDestination);
	TestTrue(TEXT("Frozen exact destination can dispatch once"),
				  ExactResult.AccessContext->TryDispatch(ExactApplicator, Error));
	TestEqual(TEXT("Credential scheme remains API key"), ExactApplicator.ObservedScheme, EUnrealAIAuthScheme::ApiKey);
	TestEqual(TEXT("Credential byte count is preserved without string conversion"), ExactApplicator.ObservedBytes, 4);
	TestEqual(TEXT("Credential bytes cross only the dispatcher seam"), ExactApplicator.ObservedSum, uint32(10));
	TestEqual(TEXT("Rejected destination resolution publishes exactly once"), Sink->GetCount(), 1);
	TestEqual(TEXT("Exact destination resolution publishes exactly once"), ExactSink->GetCount(), 1);

	FUnrealAICancellationSource RevocationCancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> RevocationSink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RevocationHandle;
	TestTrue(TEXT("Another one-shot lease resolves before quarantine"),
				  Broker.StartResolve(MakeBrokerTestRequest(200, Connection.ConnectionAlias), RevocationSink,
									  RevocationCancellation.GetToken(), RevocationHandle, AccessError));
	FUnrealAICredentialResult RevocationResult;
	TestTrue(TEXT("Second lease publishes success"), RevocationSink->WaitAndTake(RevocationResult));
	Broker.InvalidateAccount(TestAuthProfile, Account);
	FBrokerTestCredentialApplicator RevokedApplicator(Connection.CredentialDestination);
	TestFalse(TEXT("Exact account invalidation revokes an already published lease"),
				   RevocationResult.AccessContext->TryDispatch(RevokedApplicator, Error));

	FUnrealAICancellationSource RotationCancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> RotationSink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RotationHandle;
	TestTrue(TEXT("Lease invalidation permits a later resolve to load a rotated secure-store revision"),
				  Broker.StartResolve(MakeBrokerTestRequest(300, Connection.ConnectionAlias), RotationSink,
									  RotationCancellation.GetToken(), RotationHandle, AccessError));
	FUnrealAICredentialResult RotationResult;
	TestTrue(TEXT("Post-invalidation rotation resolve succeeds"), RotationSink->WaitAndTake(RotationResult));

	Broker.QuarantineAccount(TestAuthProfile, Account);
	FBrokerTestCredentialApplicator QuarantinedApplicator(Connection.CredentialDestination);
	TestFalse(TEXT("Quarantine revokes the last published account lease"),
				   RotationResult.AccessContext->TryDispatch(QuarantinedApplicator, Error));
	FUnrealAICancellationSource QuarantinedCancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> QuarantinedSink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> QuarantinedHandle;
	TestFalse(TEXT("Quarantined account rejects future resolution until explicit broker reconfiguration"),
				   Broker.StartResolve(MakeBrokerTestRequest(400, Connection.ConnectionAlias), QuarantinedSink,
									   QuarantinedCancellation.GetToken(), QuarantinedHandle, AccessError));
	TestFalse(TEXT("Quarantine rejection returns no request handle"), QuarantinedHandle.IsValid());
	TestEqual(TEXT("Quarantine rejection emits no asynchronous terminal"), QuarantinedSink->GetCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyCredentialBrokerExactConnectionBindingTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.KeyAliasRequiresExactConnection",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyCredentialBrokerExactConnectionBindingTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeBrokerTestAccount(41);
	const FUnrealAISecretHandle PrimaryHandle = MakeBrokerTestHandle(51);
	const FUnrealAISecretHandle SecondaryHandle = MakeBrokerTestHandle(61);
	FUnrealAIConnectionDescriptor Primary =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_exact_primary"), Account, Error);
	FUnrealAIConnectionDescriptor Secondary =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_exact_secondary"), Account, Error);
	Secondary.EndpointProfileId = TEXT("tests.api_key_endpoint_secondary");
	Secondary.CredentialDestination.EndpointPolicyRevision = 12;
	Secondary.CredentialDestination.Audience = TEXT("openai-compatible-secondary");
	TestTrue(TEXT("Secondary origin parses"),
				  FUnrealAIEndpointOrigin::TryParse(TEXT("https://secondary.example.com"), false,
														 Secondary.CredentialDestination.EndpointOrigin, Error));
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeBrokerDistinctEndpointSnapshot(Primary, Secondary, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 28), 12.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(TestStoreName, 4);
	TestTrue(TEXT("Primary secret fixture is stored"),
				  PrepopulateBrokerTestSecret(*Memory, Clock, PrimaryHandle, {1, 3, 5, 7}, Error));
	TestTrue(TEXT("Secondary secret fixture is stored"),
				  PrepopulateBrokerTestSecret(*Memory, Clock, SecondaryHandle, {2, 4, 6, 8}, Error));
	const TSharedRef<FBrokerTestThreadRecordingSecretStore, ESPMode::ThreadSafe> RecordingStore =
		MakeShared<FBrokerTestThreadRecordingSecretStore, ESPMode::ThreadSafe>(Memory);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = RecordingStore;
	FUnrealAIApiKeyCredentialBroker Broker(Connections, Store, Clock);
	TestFalse(TEXT("An unknown connection alias cannot receive a credential binding"),
				   RegisterBrokerTestBinding(Broker, TEXT("tests.api_exact_unknown"), Account, PrimaryHandle, Error));
	TestFalse(TEXT("An exact connection rejects account drift at binding time"),
				   RegisterBrokerTestBinding(Broker, Primary.ConnectionAlias, MakeBrokerTestAccount(42), PrimaryHandle,
											 Error));
	TestTrue(TEXT("Primary exact connection binding registers"),
				  RegisterBrokerTestBinding(Broker, Primary.ConnectionAlias, Account, PrimaryHandle, Error));
	TestFalse(TEXT("An exact connection alias cannot be rebound to another secret"),
				   RegisterBrokerTestBinding(Broker, Primary.ConnectionAlias, Account, SecondaryHandle, Error));

	FUnrealAICancellationSource Cancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
	FUnrealAIProviderAccessError AccessError;
	TestFalse(TEXT("Unbound secondary destination is rejected before secure-store or network work"),
				   Broker.StartResolve(MakeBrokerTestRequest(71, Secondary.ConnectionAlias), Sink,
									   Cancellation.GetToken(), RequestHandle, AccessError));
	TestEqual(TEXT("Unbound secondary rejection is a closed access-profile error"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	TestEqual(TEXT("Rejected destination performs no secure-store load"), RecordingStore->LoadCalls.Load(), 0);

	TestTrue(TEXT("An explicit secondary exact connection binding registers"),
				  RegisterBrokerTestBinding(Broker, Secondary.ConnectionAlias, Account, SecondaryHandle, Error));
	TestTrue(TEXT("Explicitly bound secondary destination is admitted"),
				  Broker.StartResolve(MakeBrokerTestRequest(81, Secondary.ConnectionAlias), Sink,
									  Cancellation.GetToken(), RequestHandle, AccessError));
	FUnrealAICredentialResult Result;
	TestTrue(TEXT("Explicit secondary resolve publishes"), Sink->WaitAndTake(Result));
	TestEqual(TEXT("Explicit secondary resolve succeeds"), Result.Kind, EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("Explicit secondary resolve returns an opaque one-shot context"), Result.AccessContext.IsValid());
	FBrokerTestCredentialApplicator SecondaryApplicator(Secondary.CredentialDestination);
	TestTrue(TEXT("The secondary destination dispatches only its explicitly bound secret"),
				  Result.AccessContext->TryDispatch(SecondaryApplicator, Error));
	TestEqual(TEXT("Secondary dispatch preserves its distinct credential byte count"),
				   SecondaryApplicator.ObservedBytes, 4);
	TestEqual(TEXT("Secondary dispatch preserves its distinct credential fingerprint"), SecondaryApplicator.ObservedSum,
				   uint32(20));
	Broker.BeginShutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyCredentialBrokerInvalidationTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.ExactAccountInvalidationDiscardsLateSecret",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyCredentialBrokerInvalidationTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FName AuthProfileB = TEXT("tests.api_key_profile_b");
	const FUnrealAIAccessAccountId AccountA = MakeBrokerTestAccount(101);
	const FUnrealAIAccessAccountId AccountB = MakeBrokerTestAccount(201);
	const FUnrealAIConnectionDescriptor ConnectionA =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_account_a"), AccountA, Error);
	const FUnrealAIConnectionDescriptor ConnectionB =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_account_b"), AccountB, Error, AuthProfileB);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeBrokerTestConnectionSnapshot({ConnectionA, ConnectionB}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 20.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FBrokerTestBlockingSecretStoreControl, ESPMode::ThreadSafe> Control =
		MakeShared<FBrokerTestBlockingSecretStoreControl, ESPMode::ThreadSafe>();
	const TSharedRef<FBrokerTestBlockingSecretStore, ESPMode::ThreadSafe> BlockingStore =
		MakeShared<FBrokerTestBlockingSecretStore, ESPMode::ThreadSafe>(Control);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = BlockingStore;
	FUnrealAIApiKeyCredentialBrokerConfig Config;
	Config.MaxConcurrentStoreOperations = 2;
	FUnrealAIApiKeyCredentialBroker Broker(Connections, Store, Clock, Config);
	TestTrue(TEXT("Account A binding registers"),
				  RegisterBrokerTestBinding(Broker, ConnectionA.ConnectionAlias, AccountA, MakeBrokerTestHandle(301),
											Error));
	TestTrue(TEXT("Account B binding registers"),
				  RegisterBrokerTestBinding(Broker, ConnectionB.ConnectionAlias, AccountB, MakeBrokerTestHandle(401),
											Error, AuthProfileB));

	FUnrealAICancellationSource CancellationA;
	FUnrealAICancellationSource CancellationB;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> SinkA =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> SinkB =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> HandleA;
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> HandleB;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Account A load is admitted"),
				  Broker.StartResolve(MakeBrokerTestRequest(501, ConnectionA.ConnectionAlias), SinkA,
									  CancellationA.GetToken(), HandleA, AccessError));
	TestTrue(TEXT("Account B load is admitted"),
				  Broker.StartResolve(MakeBrokerTestRequest(601, ConnectionB.ConnectionAlias), SinkB,
									  CancellationB.GetToken(), HandleB, AccessError));
	TestTrue(TEXT("Both physical loads enter the hung backend"),
				  WaitForBrokerTestCondition([&Control]() { return Control->EnteredCount.Load() == 2; }));
	TestFalse(TEXT("Hung secure-store calls execute off the game thread"), Control->bObservedGameThread.Load());

	Broker.InvalidateAccount(TestAuthProfile, AccountA);
	FUnrealAICredentialResult ResultA;
	TestTrue(TEXT("Exact account invalidation settles account A immediately"), SinkA->WaitAndTake(ResultA));
	TestEqual(TEXT("Invalidated account terminal is cancelled"), ResultA.Kind,
				   EUnrealAICredentialResultKind::Cancelled);
	TestEqual(TEXT("Account B remains pending"), SinkB->GetCount(), 0);
	TestEqual(TEXT("Logical invalidation does not pretend hung physical work stopped"),
				   Broker.GetPhysicalStoreOperationCount(), 2);

	Control->Release->Trigger();
	FUnrealAICredentialResult ResultB;
	TestTrue(TEXT("Unaffected account publishes success after physical completion"), SinkB->WaitAndTake(ResultB));
	TestEqual(TEXT("Unaffected account succeeds"), ResultB.Kind, EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("Both physical operations eventually reconcile"),
				  WaitForBrokerTestCondition([&Broker]() { return Broker.GetPhysicalStoreOperationCount() == 0; }));
	TestEqual(TEXT("Late secret from invalidated account is discarded without reopening terminal"), SinkA->GetCount(),
				   1);
	TestEqual(TEXT("Late successful load takes the explicit secret-wipe path"), Broker.GetDiscardedLateSecretCount(),
				   1);

	FUnrealAICancellationSource RotationCancellationA;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> RotationSinkA =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RotationHandleA;
	TestTrue(TEXT("Exact lease invalidation still permits account A key rotation"),
				  Broker.StartResolve(MakeBrokerTestRequest(701, ConnectionA.ConnectionAlias), RotationSinkA,
									  RotationCancellationA.GetToken(), RotationHandleA, AccessError));
	FUnrealAICredentialResult RotationResultA;
	TestTrue(TEXT("Rotated account A resolve succeeds"), RotationSinkA->WaitAndTake(RotationResultA));
	Broker.InvalidateAuthProfile(TestAuthProfile);
	FBrokerTestCredentialApplicator ProfileRevokedApplicatorA(ConnectionA.CredentialDestination);
	TestFalse(TEXT("Profile invalidation revokes the matching profile's published lease"),
				   RotationResultA.AccessContext->TryDispatch(ProfileRevokedApplicatorA, Error));
	TestTrue(TEXT("Profile invalidation does not revoke another profile's lease"), ResultB.AccessContext->IsValid());

	FUnrealAICancellationSource PostProfileCancellationA;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> PostProfileSinkA =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> PostProfileHandleA;
	TestTrue(TEXT("Profile invalidation remains rotation-safe for a later account A resolve"),
				  Broker.StartResolve(MakeBrokerTestRequest(751, ConnectionA.ConnectionAlias), PostProfileSinkA,
									  PostProfileCancellationA.GetToken(), PostProfileHandleA, AccessError));
	FUnrealAICredentialResult PostProfileResultA;
	TestTrue(TEXT("Post-profile-invalidation account A resolve succeeds"),
				  PostProfileSinkA->WaitAndTake(PostProfileResultA));
	Broker.QuarantineAccount(TestAuthProfile, AccountA);
	FBrokerTestCredentialApplicator QuarantinedApplicatorA(ConnectionA.CredentialDestination);
	TestFalse(TEXT("Account quarantine revokes the rotated account A lease"),
				   PostProfileResultA.AccessContext->TryDispatch(QuarantinedApplicatorA, Error));

	FUnrealAICancellationSource QuarantinedCancellationA;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> QuarantinedSinkA =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> QuarantinedHandleA;
	TestFalse(TEXT("Account quarantine blocks later account A resolves"),
				   Broker.StartResolve(MakeBrokerTestRequest(801, ConnectionA.ConnectionAlias), QuarantinedSinkA,
									   QuarantinedCancellationA.GetToken(), QuarantinedHandleA, AccessError));
	TestFalse(TEXT("Quarantined account A returns no request handle"), QuarantinedHandleA.IsValid());

	FBrokerTestCredentialApplicator ApplicatorB(ConnectionB.CredentialDestination);
	TestTrue(TEXT("Exact invalidation does not revoke another account's lease"),
				  ResultB.AccessContext->TryDispatch(ApplicatorB, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyCredentialBrokerBoundedLifecycleTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.TimeoutCapacityAndShutdownTerminalOnce",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyCredentialBrokerBoundedLifecycleTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeBrokerTestAccount(701);
	const FUnrealAIConnectionDescriptor Connection =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_bounded"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeBrokerTestConnectionSnapshot({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 30.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FBrokerTestBlockingSecretStoreControl, ESPMode::ThreadSafe> Control =
		MakeShared<FBrokerTestBlockingSecretStoreControl, ESPMode::ThreadSafe>();
	const TSharedRef<FBrokerTestBlockingSecretStore, ESPMode::ThreadSafe> BlockingStore =
		MakeShared<FBrokerTestBlockingSecretStore, ESPMode::ThreadSafe>(Control);
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = BlockingStore;
	FUnrealAIApiKeyCredentialBrokerConfig Config;
	Config.MaxConcurrentStoreOperations = 1;
	FUnrealAIApiKeyCredentialBroker Broker(Connections, Store, Clock, Config);
	TestTrue(
		TEXT("Bounded-lifecycle binding registers"),
			 RegisterBrokerTestBinding(Broker, Connection.ConnectionAlias, Account, MakeBrokerTestHandle(801), Error));

	FUnrealAICancellationSource Cancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> Sink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(TEXT("Hung load is admitted"),
				  Broker.StartResolve(MakeBrokerTestRequest(901, Connection.ConnectionAlias, 1.0f), Sink,
									  Cancellation.GetToken(), RequestHandle, AccessError));
	TestTrue(TEXT("Hung physical load enters"), Control->Entered->Wait(2000));

	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> OverflowSink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> OverflowHandle;
	FUnrealAICancellationSource OverflowCancellation;
	TestFalse(TEXT("A hung physical call retains its bounded admission slot"),
				   Broker.StartResolve(MakeBrokerTestRequest(1001, Connection.ConnectionAlias), OverflowSink,
									   OverflowCancellation.GetToken(), OverflowHandle, AccessError));
	TestFalse(TEXT("Synchronous capacity rejection returns no request handle"), OverflowHandle.IsValid());
	TestEqual(TEXT("Capacity rejection does not emit an asynchronous terminal"), OverflowSink->GetCount(), 0);
	TestEqual(TEXT("Physical admission capacity has a typed busy category"), AccessError.Category,
				   EUnrealAIErrorCategory::Busy);
	TestEqual(TEXT("Physical admission capacity has a typed refresh-capacity code"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity);
	TestTrue(TEXT("Physical admission capacity is retryable"), AccessError.bRetryable);

	MutableClock->Advance(FTimespan::FromSeconds(1));
	FUnrealAICredentialResult TimedOut;
	TestTrue(TEXT("Logical timeout settles without waiting for the hung backend"), Sink->WaitAndTake(TimedOut));
	TestEqual(TEXT("Timeout has the exact terminal kind"), TimedOut.Kind, EUnrealAICredentialResultKind::TimedOut);
	TestEqual(TEXT("Timeout leaves the physical operation accounted until reconciliation"),
				   Broker.GetPhysicalStoreOperationCount(), 1);

	TAtomic<int32> TerminalRaceCalls{0};
	ParallelFor(32,
				[&Broker, &RequestHandle, &TerminalRaceCalls](const int32 Index)
				{
					if ((Index & 1) == 0)
					{
						RequestHandle->Cancel();
					}
					else
					{
						Broker.BeginShutdown();
					}
					++TerminalRaceCalls;
				});
	TestEqual(TEXT("All terminal-race callers returned"), TerminalRaceCalls.Load(), 32);
	TestTrue(TEXT("Shutdown is monotonic"), Broker.IsShutdown());
	TestEqual(TEXT("Timeout, cancel, and shutdown races publish exactly once"), Sink->GetCount(), 1);

	Control->Release->Trigger();
	TestTrue(TEXT("Late successful secret is wiped/discarded and physical count reconciles"),
				  WaitForBrokerTestCondition([&Broker]() { return Broker.GetPhysicalStoreOperationCount() == 0; }));
	TestEqual(TEXT("Late timeout completion takes the explicit secret-wipe path"), Broker.GetDiscardedLateSecretCount(),
				   1);
	TestEqual(TEXT("Late completion cannot reopen the terminal"), Sink->GetCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyCredentialBrokerLeaseCapacityTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.LeaseCapacityExpiryAndConnectionInvalidation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyCredentialBrokerLeaseCapacityTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeBrokerTestAccount(1701);
	const FUnrealAISecretHandle SecretHandle = MakeBrokerTestHandle(1801);
	const FUnrealAIConnectionDescriptor Connection =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_lease_capacity"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeBrokerTestConnectionSnapshot({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 50.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(TestStoreName, 4);
	TestTrue(TEXT("Lease-capacity fixture is stored"),
				  PrepopulateBrokerTestSecret(*Memory, Clock, SecretHandle, {5, 6, 7, 8}, Error));
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = Memory;
	FUnrealAIApiKeyCredentialBrokerConfig Config;
	Config.MaxActiveLeases = 1;
	Config.LeaseLifetimeSeconds = 1.0;
	FUnrealAIApiKeyCredentialBroker Broker(Connections, Store, Clock, Config);
	TestTrue(TEXT("Lease-capacity binding registers"),
				  RegisterBrokerTestBinding(Broker, Connection.ConnectionAlias, Account, SecretHandle, Error));

	auto Resolve =
		[&Broker, &Connection](const uint32 Seed,
							   const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> &Sink,
							   TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> &OutHandle,
							   FUnrealAIProviderAccessError &OutError, FUnrealAICancellationSource &Cancellation)
	{
		return Broker.StartResolve(MakeBrokerTestRequest(Seed, Connection.ConnectionAlias), Sink,
								   Cancellation.GetToken(), OutHandle, OutError);
	};

	FUnrealAIProviderAccessError AccessError;
	FUnrealAICancellationSource FirstCancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> FirstSink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> FirstHandle;
	TestTrue(TEXT("First bounded lease resolve is admitted"),
				  Resolve(1901, FirstSink, FirstHandle, AccessError, FirstCancellation));
	FUnrealAICredentialResult FirstResult;
	TestTrue(TEXT("First bounded lease resolves"), FirstSink->WaitAndTake(FirstResult));
	TestEqual(TEXT("First bounded lease succeeds"), FirstResult.Kind, EUnrealAICredentialResultKind::Succeeded);
	TestTrue(TEXT("First physical operation reconciles before the next admission"),
				  WaitForBrokerTestCondition([&Broker]() { return Broker.GetPhysicalStoreOperationCount() == 0; }));
	TestEqual(TEXT("A caller-retained completed handle does not retain broker operation tracking"),
				   Broker.GetTrackedOperationCount(), 0);
	TestEqual(TEXT("One bounded freshness lease is retained"), Broker.GetActiveLeaseCount(), 1);

	FUnrealAICancellationSource CapacityCancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> CapacitySink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> CapacityHandle;
	TestTrue(TEXT("A store load may be admitted before lease capacity is known"),
				  Resolve(2001, CapacitySink, CapacityHandle, AccessError, CapacityCancellation));
	FUnrealAICredentialResult CapacityResult;
	TestTrue(TEXT("Lease-capacity exhaustion publishes a terminal"), CapacitySink->WaitAndTake(CapacityResult));
	TestEqual(TEXT("Lease-capacity exhaustion is a failed credential result"), CapacityResult.Kind,
				   EUnrealAICredentialResultKind::Failed);
	TestEqual(TEXT("Lease-capacity exhaustion has a typed busy category"), CapacityResult.Error.Category,
				   EUnrealAIErrorCategory::Busy);
	TestEqual(TEXT("Lease-capacity exhaustion has a typed refresh-capacity code"), CapacityResult.Error.Code,
				   EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity);
	TestEqual(TEXT("Lease-capacity exhaustion publishes exactly once"), CapacitySink->GetCount(), 1);
	TestTrue(TEXT("Capacity-failed physical operation reconciles"),
				  WaitForBrokerTestCondition([&Broker]() { return Broker.GetPhysicalStoreOperationCount() == 0; }));

	MutableClock->Advance(FTimespan::FromSeconds(1));
	TestEqual(TEXT("Expired freshness leases release bounded capacity"), Broker.GetActiveLeaseCount(), 0);
	FBrokerTestCredentialApplicator ExpiredApplicator(Connection.CredentialDestination);
	TestFalse(TEXT("Expired context cannot expose credential bytes"),
				   FirstResult.AccessContext->TryDispatch(ExpiredApplicator, Error));

	FUnrealAICancellationSource RecoveredCancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> RecoveredSink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RecoveredHandle;
	TestTrue(TEXT("Freshness expiry restores lease admission"),
				  Resolve(2101, RecoveredSink, RecoveredHandle, AccessError, RecoveredCancellation));
	FUnrealAICredentialResult RecoveredResult;
	TestTrue(TEXT("Post-expiry resolve succeeds"), RecoveredSink->WaitAndTake(RecoveredResult));
	Broker.InvalidateConnection(Connection.ConnectionAlias);
	FBrokerTestCredentialApplicator InvalidatedApplicator(Connection.CredentialDestination);
	TestFalse(TEXT("Exact connection invalidation revokes the published lease"),
				   RecoveredResult.AccessContext->TryDispatch(InvalidatedApplicator, Error));

	TestTrue(TEXT("Invalidated physical operation reconciles before rotation"),
				  WaitForBrokerTestCondition([&Broker]() { return Broker.GetPhysicalStoreOperationCount() == 0; }));
	FUnrealAICancellationSource RotationCancellation;
	const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> RotationSink =
		MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
	TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RotationHandle;
	TestTrue(TEXT("Connection invalidation permits a later descriptor-revision resolve"),
				  Resolve(2201, RotationSink, RotationHandle, AccessError, RotationCancellation));
	FUnrealAICredentialResult RotationResult;
	TestTrue(TEXT("Post-connection-invalidation resolve succeeds"), RotationSink->WaitAndTake(RotationResult));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAICredentialLeaseReleaseCallbackTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.LeaseReleaseCallbackExactlyOnceAcrossMoves",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAICredentialLeaseReleaseCallbackTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIConnectionDescriptor Connection =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_release_callback"), MakeBrokerTestAccount(2301), Error);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 60.0);
	FUnrealAICredentialFreshnessSource Freshness;

	int32 ReplacedReleaseCount = 0;
	int32 AdoptedReleaseCount = 0;
	{
		FUnrealAISecretValue ReplacedSecret;
		TestTrue(TEXT("Move-replacement fixture secret is created"),
					  MakeBrokerTestSecret({1, 2, 3, 4}, ReplacedSecret, Error));
		FUnrealAICredentialLease Destination;
		TUniqueFunction<void()> ReplacedRelease = [&ReplacedReleaseCount]() { ++ReplacedReleaseCount; };
		TestTrue(TEXT("Move-replacement destination lease is created"),
					  FUnrealAICredentialLease::TryCreate(Connection.CredentialDestination, Clock, Freshness.GetToken(),
														  30.0, {}, MoveTemp(ReplacedSecret), Destination, Error,
														  MoveTemp(ReplacedRelease)));

		FUnrealAISecretValue AdoptedSecret;
		TestTrue(TEXT("Move-adoption fixture secret is created"),
					  MakeBrokerTestSecret({5, 6, 7, 8}, AdoptedSecret, Error));
		FUnrealAICredentialLease Source;
		TUniqueFunction<void()> AdoptedRelease = [&AdoptedReleaseCount]() { ++AdoptedReleaseCount; };
		TestTrue(TEXT("Move-adoption source lease is created"),
					  FUnrealAICredentialLease::TryCreate(Connection.CredentialDestination, Clock, Freshness.GetToken(),
														  30.0, {}, MoveTemp(AdoptedSecret), Source, Error,
														  MoveTemp(AdoptedRelease)));

		Destination = MoveTemp(Source);
		TestEqual(TEXT("Move replacement releases the destination's prior callback once"), ReplacedReleaseCount, 1);
		TestEqual(TEXT("Move assignment transfers rather than runs the source callback"), AdoptedReleaseCount, 0);
		Source.Reset();
		TestEqual(TEXT("Resetting a moved-from lease does not run the transferred callback"), AdoptedReleaseCount, 0);
		Destination.Reset();
		TestEqual(TEXT("Reset runs the transferred callback exactly once"), AdoptedReleaseCount, 1);
		Destination.Reset();
		TestEqual(TEXT("Repeated reset cannot run the callback again"), AdoptedReleaseCount, 1);
	}
	TestEqual(TEXT("Destruction after reset cannot rerun the replacement callback"), ReplacedReleaseCount, 1);
	TestEqual(TEXT("Destruction after reset cannot rerun the adopted callback"), AdoptedReleaseCount, 1);

	int32 DestructorReleaseCount = 0;
	{
		FUnrealAISecretValue DestructorSecret;
		TestTrue(TEXT("Destructor fixture secret is created"),
					  MakeBrokerTestSecret({9, 10, 11, 12}, DestructorSecret, Error));
		FUnrealAICredentialLease DestructorLease;
		TUniqueFunction<void()> DestructorRelease = [&DestructorReleaseCount]() { ++DestructorReleaseCount; };
		TestTrue(TEXT("Destructor-owned lease is created"),
					  FUnrealAICredentialLease::TryCreate(Connection.CredentialDestination, Clock, Freshness.GetToken(),
														  30.0, {}, MoveTemp(DestructorSecret), DestructorLease, Error,
														  MoveTemp(DestructorRelease)));
	}
	TestEqual(TEXT("Lease destruction runs an unconsumed callback exactly once"), DestructorReleaseCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyCredentialBrokerConsumedLeaseCapacityTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.ConsumedOneShotLeasesReleaseCapacityImmediately",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyCredentialBrokerConsumedLeaseCapacityTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIAccessAccountId Account = MakeBrokerTestAccount(2401);
	const FUnrealAISecretHandle SecretHandle = MakeBrokerTestHandle(2501);
	const FUnrealAIConnectionDescriptor Connection =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_consumed_capacity"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeBrokerTestConnectionSnapshot({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 70.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;
	const TSharedRef<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe> Memory =
		MakeShared<FUnrealAIMemorySecretStore, ESPMode::ThreadSafe>(TestStoreName, 4);
	TestTrue(TEXT("Consumed-capacity fixture is stored"),
				  PrepopulateBrokerTestSecret(*Memory, Clock, SecretHandle, {13, 14, 15, 16}, Error));
	const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = Memory;
	FUnrealAIApiKeyCredentialBrokerConfig Config;
	Config.MaxConcurrentStoreOperations = 1;
	Config.MaxActiveLeases = 1;
	Config.LeaseLifetimeSeconds = 60.0;
	FUnrealAIApiKeyCredentialBroker Broker(Connections, Store, Clock, Config);
	TestTrue(TEXT("Consumed-capacity binding registers"),
				  RegisterBrokerTestBinding(Broker, Connection.ConnectionAlias, Account, SecretHandle, Error));

	const double InitialMonotonicSeconds = MutableClock->MonotonicSeconds();
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FUnrealAICancellationSource Cancellation;
		const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
		FUnrealAIProviderAccessError AccessError;
		if (!TestTrue(TEXT("Successive one-shot resolve is admitted without waiting for lease expiry"),
						   Broker.StartResolve(MakeBrokerTestRequest(2601u + static_cast<uint32>(Index * 10),
																	 Connection.ConnectionAlias),
											   Sink, Cancellation.GetToken(), RequestHandle, AccessError)))
		{
			return false;
		}

		FUnrealAICredentialResult Result;
		if (!TestTrue(TEXT("Successive one-shot resolve publishes a terminal"), Sink->WaitAndTake(Result)))
		{
			return false;
		}
		if (!TestEqual(TEXT("Successive one-shot resolve succeeds"), Result.Kind,
							EUnrealAICredentialResultKind::Succeeded) ||
					   !TestTrue(TEXT("Successive resolve carries its one-shot access context"),
									  Result.AccessContext.IsValid()))
		{
			return false;
		}

		FBrokerTestCredentialApplicator Applicator(Connection.CredentialDestination);
		TestTrue(TEXT("Successive one-shot context dispatches its exact destination"),
					  Result.AccessContext->TryDispatch(Applicator, Error));
		TestEqual(TEXT("Successive one-shot dispatch preserves credential bytes"), Applicator.ObservedBytes, 4);
		TestTrue(TEXT("Successive physical store operation reconciles before the next resolve"),
					  WaitForBrokerTestCondition([&Broker]() { return Broker.GetPhysicalStoreOperationCount() == 0; }));
		TestEqual(TEXT("Consumed lease cleanup does not require clock advancement"), MutableClock->MonotonicSeconds(),
					   InitialMonotonicSeconds);
	}

	TestEqual(TEXT("Consumed one-shot lease releases the final bounded slot"), Broker.GetActiveLeaseCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIApiKeyCredentialBrokerFaultProjectionTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.TypedSecretStoreFaultProjection",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIApiKeyCredentialBrokerFaultProjectionTest::RunTest(const FString &Parameters)
{
	struct FFaultCase final
	{
		EUnrealAISecretStoreResult StoreResult;
		EUnrealAICredentialResultKind CredentialResult;
		EUnrealAIErrorCategory Category;
		EUnrealAIProviderAccessErrorCode Code;
	};
	const TArray<FFaultCase> Cases{
		{EUnrealAISecretStoreResult::NotFound, EUnrealAICredentialResultKind::Failed, EUnrealAIErrorCategory::NotFound,
		 EUnrealAIProviderAccessErrorCode::SecretNotFound},
		{EUnrealAISecretStoreResult::Conflict, EUnrealAICredentialResultKind::Failed,
		 EUnrealAIErrorCategory::VersionMismatch, EUnrealAIProviderAccessErrorCode::SecretRevisionConflict},
		{EUnrealAISecretStoreResult::Locked, EUnrealAICredentialResultKind::Failed, EUnrealAIErrorCategory::Busy,
		 EUnrealAIProviderAccessErrorCode::SecretStoreLocked},
		{EUnrealAISecretStoreResult::Denied, EUnrealAICredentialResultKind::Failed,
		 EUnrealAIErrorCategory::PolicyDenied, EUnrealAIProviderAccessErrorCode::SecretStoreDenied},
		{EUnrealAISecretStoreResult::Unavailable, EUnrealAICredentialResultKind::Failed,
		 EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable},
		{EUnrealAISecretStoreResult::NotSupported, EUnrealAICredentialResultKind::Failed,
		 EUnrealAIErrorCategory::UnsupportedCapability, EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported},
		{EUnrealAISecretStoreResult::Corrupt, EUnrealAICredentialResultKind::Failed,
		 EUnrealAIErrorCategory::Persistence, EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt},
		{EUnrealAISecretStoreResult::Cancelled, EUnrealAICredentialResultKind::Cancelled,
		 EUnrealAIErrorCategory::Cancelled, EUnrealAIProviderAccessErrorCode::SecretStoreCancelled},
		{EUnrealAISecretStoreResult::TimedOut, EUnrealAICredentialResultKind::TimedOut, EUnrealAIErrorCategory::Timeout,
		 EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut},
		{EUnrealAISecretStoreResult::Failed, EUnrealAICredentialResultKind::Failed, EUnrealAIErrorCategory::Internal,
		 EUnrealAIProviderAccessErrorCode::Internal}};

	FString Error;
	const FUnrealAIAccessAccountId Account = MakeBrokerTestAccount(1101);
	const FUnrealAISecretHandle SecretHandle = MakeBrokerTestHandle(1201);
	const FUnrealAIConnectionDescriptor Connection =
		MakeBrokerTestApiKeyConnection(TEXT("tests.api_faults"), Account, Error);
	const TSharedRef<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> Connections =
		MakeBrokerTestConnectionSnapshot({Connection}, Error);
	const TSharedRef<FUnrealAITestClock, ESPMode::ThreadSafe> MutableClock =
		MakeShared<FUnrealAITestClock, ESPMode::ThreadSafe>(FDateTime(2026, 7, 21), 40.0);
	const TSharedRef<const IUnrealAIClock, ESPMode::ThreadSafe> Clock = MutableClock;

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FFaultCase &Fault = Cases[Index];
		const TSharedRef<FBrokerTestFaultSecretStore, ESPMode::ThreadSafe> FaultStore =
			MakeShared<FBrokerTestFaultSecretStore, ESPMode::ThreadSafe>(Fault.StoreResult);
		const TSharedRef<IUnrealAISecretStore, ESPMode::ThreadSafe> Store = FaultStore;
		FUnrealAIApiKeyCredentialBroker Broker(Connections, Store, Clock);
		TestTrue(FString::Printf(TEXT("Fault case %d binding registers"), Index),
								 RegisterBrokerTestBinding(Broker, Connection.ConnectionAlias, Account, SecretHandle,
														   Error));
		FUnrealAICancellationSource Cancellation;
		const TSharedRef<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe> Sink =
			MakeShared<FBrokerTestRecordingCredentialSink, ESPMode::ThreadSafe>();
		TSharedPtr<IUnrealAICredentialRequestHandle, ESPMode::ThreadSafe> RequestHandle;
		FUnrealAIProviderAccessError AccessError;
		TestTrue(FString::Printf(TEXT("Fault case %d is admitted"), Index),
								 Broker.StartResolve(MakeBrokerTestRequest(1301 + static_cast<uint32>(Index * 10),
																		   Connection.ConnectionAlias),
													 Sink, Cancellation.GetToken(), RequestHandle, AccessError));
		FUnrealAICredentialResult Result;
		TestTrue(FString::Printf(TEXT("Fault case %d publishes"), Index), Sink->WaitAndTake(Result));
		TestEqual(FString::Printf(TEXT("Fault case %d terminal kind is preserved"), Index), Result.Kind,
								  Fault.CredentialResult);
		TestEqual(FString::Printf(TEXT("Fault case %d category is typed"), Index), Result.Error.Category,
								  Fault.Category);
		TestEqual(FString::Printf(TEXT("Fault case %d code is typed"), Index), Result.Error.Code, Fault.Code);
		TestEqual(FString::Printf(TEXT("Fault case %d is terminal-once"), Index), Sink->GetCount(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAICredentialTypedStoreResultShapeTest,
								 "UnrealAI.Auth.ApiKeyCredentialBroker.TypedStoreResultShape",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAICredentialTypedStoreResultShapeTest::RunTest(const FString &Parameters)
{
	struct FCodeCase final
	{
		EUnrealAIProviderAccessErrorCode Code;
		EUnrealAIErrorCategory Category;
	};
	const TArray<FCodeCase> FailedCodes{
		{EUnrealAIProviderAccessErrorCode::CredentialFailed, EUnrealAIErrorCategory::Provider},
		{EUnrealAIProviderAccessErrorCode::CredentialEntitlementDenied, EUnrealAIErrorCategory::PolicyDenied},
		{EUnrealAIProviderAccessErrorCode::CredentialRefreshCapacity, EUnrealAIErrorCategory::Busy},
		{EUnrealAIProviderAccessErrorCode::PartnerGated, EUnrealAIErrorCategory::UnsupportedCapability},
		{EUnrealAIProviderAccessErrorCode::AccessProfileNotReady, EUnrealAIErrorCategory::NotAuthorized},
		{EUnrealAIProviderAccessErrorCode::UnsupportedCapability, EUnrealAIErrorCategory::UnsupportedCapability},
		{EUnrealAIProviderAccessErrorCode::InvalidSecretStoreContext, EUnrealAIErrorCategory::InvalidArgument},
		{EUnrealAIProviderAccessErrorCode::SecretHandleStoreMismatch, EUnrealAIErrorCategory::InvalidArgument},
		{EUnrealAIProviderAccessErrorCode::InvalidSecretStoreWrite, EUnrealAIErrorCategory::InvalidArgument},
		{EUnrealAIProviderAccessErrorCode::InvalidSecretStoreDelete, EUnrealAIErrorCategory::InvalidArgument},
		{EUnrealAIProviderAccessErrorCode::SecretNotFound, EUnrealAIErrorCategory::NotFound},
		{EUnrealAIProviderAccessErrorCode::SecretRevisionConflict, EUnrealAIErrorCategory::VersionMismatch},
		{EUnrealAIProviderAccessErrorCode::SecretStoreLocked, EUnrealAIErrorCategory::Busy},
		{EUnrealAIProviderAccessErrorCode::SecretStoreDenied, EUnrealAIErrorCategory::PolicyDenied},
		{EUnrealAIProviderAccessErrorCode::SecretStoreUnavailable, EUnrealAIErrorCategory::Persistence},
		{EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported, EUnrealAIErrorCategory::UnsupportedCapability},
		{EUnrealAIProviderAccessErrorCode::SecretStoreCorrupt, EUnrealAIErrorCategory::Persistence},
		{EUnrealAIProviderAccessErrorCode::SecretStoreCapacity, EUnrealAIErrorCategory::Busy},
		{EUnrealAIProviderAccessErrorCode::SecretCopyFailed, EUnrealAIErrorCategory::Internal},
		{EUnrealAIProviderAccessErrorCode::Internal, EUnrealAIErrorCategory::Internal}};

	FString Error;
	for (int32 Index = 0; Index < FailedCodes.Num(); ++Index)
	{
		FUnrealAICredentialResult Result;
		Result.RequestId.Value = FGuid(1401 + Index, 1402 + Index, 1403 + Index, 1404 + Index);
		Result.Kind = EUnrealAICredentialResultKind::Failed;
		Result.Error.Category = FailedCodes[Index].Category;
		Result.Error.Code = FailedCodes[Index].Code;
		TestTrue(FString::Printf(TEXT("Typed failed code %d validates"), Index), Result.ValidateShape(Error));
	}

	FUnrealAICredentialResult StoreCancelled;
	StoreCancelled.RequestId.Value = FGuid(1501, 1502, 1503, 1504);
	StoreCancelled.Kind = EUnrealAICredentialResultKind::Cancelled;
	StoreCancelled.Error.Category = EUnrealAIErrorCategory::Cancelled;
	StoreCancelled.Error.Code = EUnrealAIProviderAccessErrorCode::SecretStoreCancelled;
	TestTrue(TEXT("Store cancellation is a valid cancelled credential terminal"), StoreCancelled.ValidateShape(Error));

	FUnrealAICredentialResult StoreTimedOut;
	StoreTimedOut.RequestId.Value = FGuid(1601, 1602, 1603, 1604);
	StoreTimedOut.Kind = EUnrealAICredentialResultKind::TimedOut;
	StoreTimedOut.Error.Category = EUnrealAIErrorCategory::Timeout;
	StoreTimedOut.Error.Code = EUnrealAIProviderAccessErrorCode::SecretStoreTimedOut;
	TestTrue(TEXT("Store timeout is a valid timed-out credential terminal"), StoreTimedOut.ValidateShape(Error));

	StoreCancelled.Kind = EUnrealAICredentialResultKind::Failed;
	TestFalse(TEXT("Cancelled store code cannot masquerade as generic failure"), StoreCancelled.ValidateShape(Error));
	StoreTimedOut.Kind = EUnrealAICredentialResultKind::Cancelled;
	TestFalse(TEXT("Timed-out store code cannot masquerade as cancellation"), StoreTimedOut.ValidateShape(Error));
	return true;
}
