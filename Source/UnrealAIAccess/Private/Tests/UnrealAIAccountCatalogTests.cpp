// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIAccountCatalog.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Auth/UnrealAIEndpointProfileRegistry.h"
#include "Misc/AutomationTest.h"

namespace
{
const FName CatalogProviderName(TEXT("tests.catalog.oauth"));
const FName CatalogModelProviderName(TEXT("tests.catalog.model"));
const FName CatalogProfileName(TEXT("tests.catalog.profile"));
const FName CatalogEndpointName(TEXT("tests.catalog.endpoint"));
const FUnrealAIAccessAccountId CatalogAccountA{FGuid(0xaa066001, 0xaa066002, 0xaa066003, 0xaa066004)};
const FUnrealAIAccessAccountId CatalogAccountB{FGuid(0xaa066011, 0xaa066012, 0xaa066013, 0xaa066014)};

class FCatalogTestProvider final : public IUnrealAIAccountAuthProvider
{
  public:
	FCatalogTestProvider()
	{
		States.Add(CatalogAccountA, EUnrealAIAccountAuthState::Ready);
		States.Add(CatalogAccountB, EUnrealAIAccountAuthState::Ready);
	}

	FName GetProviderName() const override
	{
		return CatalogProviderName;
	}

	FUnrealAIAccountAuthCapabilities DescribeCapabilities() const override
	{
		FUnrealAIAccountAuthCapabilities Capabilities;
		Capabilities.bDeviceCode = true;
		Capabilities.bRefresh = true;
		return Capabilities;
	}

	FUnrealAIProviderAccessDescriptor DescribeAccess() const override
	{
		FUnrealAIProviderAccessDescriptor Access;
		Access.ModelProviderName = CatalogModelProviderName;
		Access.AccountAuthProviderName = CatalogProviderName;
		Access.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
		Access.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
		Access.Availability = EUnrealAIProviderAccessAvailability::Available;
		Access.SupportClassification =
			EUnrealAIProviderAccessSupportClassification::ExperimentalDirectSubscriptionCompatibility;
		return Access;
	}

	FUnrealAIAccountStatus GetStatus(const FName AuthProfileId,
									 const FUnrealAIAccessAccountId &AccountId) const override
	{
		FUnrealAIAccountStatus Status;
		Status.ProviderName = CatalogProviderName;
		Status.AuthProfileId = AuthProfileId;
		const EUnrealAIAccountAuthState *State = States.Find(AccountId);
		Status.State = State == nullptr ? EUnrealAIAccountAuthState::Invalid : *State;
		if (Status.State != EUnrealAIAccountAuthState::SignedOut && Status.State != EUnrealAIAccountAuthState::Invalid)
		{
			Status.AccountId =
				DriftedAccount.IsSet() && AccountId == CatalogAccountB ? DriftedAccount.GetValue() : AccountId;
		}
		if (Status.State == EUnrealAIAccountAuthState::Ready)
		{
			Status.AccessExpiresAtUtc = FDateTime(2026, 9, 1);
			Status.bRefreshCredentialPresent = true;
		}
		return Status;
	}

	bool StartSignIn(const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIInteractiveAuthRequest &,
					 TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe>, const FUnrealAICancellationToken &,
					 TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					 FUnrealAIProviderAccessError &OutError) override
	{
		OutHandle.Reset();
		OutError.Category = EUnrealAIErrorCategory::PolicyDenied;
		OutError.Code = EUnrealAIProviderAccessErrorCode::InvalidRequest;
		return false;
	}

	bool StartSignOut(const FUnrealAITrustedLocalAuthGesture &, const FUnrealAIAccountAuthRequest &,
					  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe>, const FUnrealAICancellationToken &,
					  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
					  FUnrealAIProviderAccessError &OutError) override
	{
		OutHandle.Reset();
		OutError.Category = EUnrealAIErrorCategory::PolicyDenied;
		OutError.Code = EUnrealAIProviderAccessErrorCode::InvalidRequest;
		return false;
	}

	void SetState(const FUnrealAIAccessAccountId &AccountId, const EUnrealAIAccountAuthState State)
	{
		States.FindOrAdd(AccountId) = State;
	}

	void SetDriftedAccount(const TOptional<FUnrealAIAccessAccountId> &AccountId)
	{
		DriftedAccount = AccountId;
	}

  private:
	TMap<FUnrealAIAccessAccountId, EUnrealAIAccountAuthState> States;
	TOptional<FUnrealAIAccessAccountId> DriftedAccount;
};

bool MakeCatalogConnection(const FUnrealAIProviderEndpointAuthority &Authority,
						   const FUnrealAIAccessAccountId &AccountId, const FName ConnectionAlias,
						   const FName PayerHandle,
						   TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> &OutConnection,
						   FString &OutError)
{
	FUnrealAIEndpointOrigin Origin;
	if (!FUnrealAIEndpointOrigin::TryParse(TEXT("https://catalog.example.test"), false, Origin, OutError))
	{
		return false;
	}
	FUnrealAIEndpointProfileRegistry Endpoints;
	TSharedPtr<const FUnrealAIEndpointProfileDescriptor, ESPMode::ThreadSafe> Endpoint;
	if (!Endpoints.RegisterProviderSubscriptionEndpoint(
			Authority, CatalogEndpointName, CatalogModelProviderName, Origin,
			TEXT("https://catalog.example.test/v1/responses"), 7, Endpoint, OutError))
	{
		return false;
	}
	FUnrealAIConnectionDescriptor Descriptor;
	Descriptor.ConnectionAlias = ConnectionAlias;
	Descriptor.EndpointProfileId = CatalogEndpointName;
	Descriptor.CredentialDestination.ModelProviderName = CatalogModelProviderName;
	Descriptor.CredentialDestination.AccountAuthProviderName = CatalogProviderName;
	Descriptor.CredentialDestination.AuthProfileId = CatalogProfileName;
	Descriptor.CredentialDestination.AccountId = AccountId;
	Descriptor.CredentialDestination.TenantRealm = TEXT("tests.catalog.tenant");
	Descriptor.CredentialDestination.BillingPrincipalId.Value = AccountId.Value;
	Descriptor.CredentialDestination.PayerHandle = PayerHandle;
	Descriptor.CredentialDestination.AuthScheme = EUnrealAIAuthScheme::OAuthBearer;
	Descriptor.CredentialDestination.BillingMode = EUnrealAIBillingMode::SubscriptionQuota;
	Descriptor.CredentialDestination.EndpointOrigin = Origin;
	Descriptor.CredentialDestination.Audience = TEXT("https://catalog.example.test/v1/responses");
	Descriptor.CredentialDestination.ConnectionRevision = 1;
	Descriptor.CredentialDestination.EndpointPolicyRevision = 7;
	FUnrealAIConnectionRegistry Connections(Endpoints.CreateSnapshot());
	return Connections.Register(Descriptor, OutConnection, OutError);
}

FUnrealAIAccountCatalogEntry MakeCatalogEntry(const FName SelectionAlias, const TCHAR *DisplayLabel,
											  const FUnrealAIAccessAccountId &AccountId, const FName ConnectionAlias)
{
	FUnrealAIAccountCatalogEntry Entry;
	Entry.SelectionAlias = SelectionAlias;
	Entry.DisplayLabel = DisplayLabel;
	Entry.ProviderName = CatalogProviderName;
	Entry.AuthProfileId = CatalogProfileName;
	Entry.AccountId = AccountId;
	Entry.ConnectionAlias = ConnectionAlias;
	return Entry;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIAccountCatalogExactMultiAccountTest,
								 "UnrealAI.Auth.AccountCatalog.ExactMultiAccountSelectionAndStaleWithdrawal",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIAccountCatalogExactMultiAccountTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const TSharedRef<FCatalogTestProvider, ESPMode::ThreadSafe> Provider =
		MakeShared<FCatalogTestProvider, ESPMode::ThreadSafe>();
	FUnrealAIAccountAuthProviderRegistry Providers;
	FUnrealAIAccountCatalogRegistry Catalog(Providers);
	FUnrealAIProviderEndpointAuthority Authority;
	FString Error;
	if (!TestTrue(TEXT("Test provider registers"), Providers.Register(Provider, Authority, Error)) ||
				  !TestTrue(TEXT("Subscription authority is minted"), Authority.IsValid()))
	{
		return false;
	}

	const FName ConnectionA(TEXT("tests.catalog.connection.a"));
	const FName ConnectionB(TEXT("tests.catalog.connection.b"));
	const FName SelectionA(TEXT("tests.catalog.account.a"));
	const FName SelectionB(TEXT("tests.catalog.account.b"));
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> DescriptorA;
	TSharedPtr<const FUnrealAIConnectionDescriptor, ESPMode::ThreadSafe> DescriptorB;
	if (!TestTrue(TEXT("Account A connection constructs"),
					   MakeCatalogConnection(Authority, CatalogAccountA, ConnectionA,
											 TEXT("tests.catalog.payer.a"), DescriptorA, Error)) ||
					   !TestTrue(TEXT("Account B connection constructs"),
									  MakeCatalogConnection(Authority, CatalogAccountB, ConnectionB,
															TEXT("tests.catalog.payer.b"), DescriptorB, Error)))
	{
		return false;
	}
	const FUnrealAIAccountCatalogEntry EntryA =
		MakeCatalogEntry(SelectionA, TEXT("Catalog account A"), CatalogAccountA, ConnectionA);
	const FUnrealAIAccountCatalogEntry EntryB =
		MakeCatalogEntry(SelectionB, TEXT("Catalog account B"), CatalogAccountB, ConnectionB);
	TestTrue(TEXT("Account A registers"), Catalog.Register(EntryA, Provider, DescriptorA.ToSharedRef(), Error));
	TestTrue(TEXT("Account B registers"), Catalog.Register(EntryB, Provider, DescriptorB.ToSharedRef(), Error));
	TestFalse(TEXT("Duplicate exact alias is never ambiguous"),
				   Catalog.Register(EntryA, Provider, DescriptorA.ToSharedRef(), Error));

	const TSharedRef<const FUnrealAIAccountCatalogSnapshot, ESPMode::ThreadSafe> Snapshot = Catalog.CreateSnapshot();
	TestEqual(TEXT("Two exact account choices are listed"), Snapshot->Num(), 2);
	const TArray<FUnrealAIAccountCatalogView> Listed = Snapshot->List();
	TestEqual(TEXT("List retains both choices"), Listed.Num(), 2);
	TestEqual(TEXT("Choices are sorted by exact alias"), Listed[0].Entry.SelectionAlias, SelectionA);

	FUnrealAIAccountCatalogView Selected;
	FUnrealAIProviderAccessError SelectionError;
	TestTrue(TEXT("Exact account A resolves"), Snapshot->ResolveReadyExact(SelectionA, Selected, SelectionError));
	TestEqual(TEXT("Exact account A cannot silently switch account"), Selected.Entry.AccountId, CatalogAccountA);
	TestEqual(TEXT("Payer disclosure remains exact"), Selected.Destination.PayerHandle,
				   FName(TEXT("tests.catalog.payer.a")));
	TestFalse(TEXT("Unknown and partial aliases fail closed"),
				   Snapshot->ResolveReadyExact(TEXT("tests.catalog.account"), Selected, SelectionError));
	TestEqual(TEXT("Unknown alias has typed not-found category"), SelectionError.Category,
				   EUnrealAIErrorCategory::NotFound);

	Provider->SetState(CatalogAccountA, EUnrealAIAccountAuthState::SignedOut);
	TestFalse(TEXT("Signed-out account never resolves for credential use"),
				   Snapshot->ResolveReadyExact(SelectionA, Selected, SelectionError));
	TestEqual(TEXT("Signed-out account has typed not-ready error"), SelectionError.Code,
				   EUnrealAIProviderAccessErrorCode::AccessProfileNotReady);
	Provider->SetState(CatalogAccountA, EUnrealAIAccountAuthState::Ready);
	Provider->SetDriftedAccount(CatalogAccountA);
	TestFalse(TEXT("A provider-side account switch is rejected"),
				   Snapshot->ResolveReadyExact(SelectionB, Selected, SelectionError));
	TestEqual(TEXT("Account drift has typed configuration error"), SelectionError.Code,
				   EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
	Provider->SetDriftedAccount({});

	TestTrue(TEXT("Exact provider can withdraw account A"), Catalog.Unregister(SelectionA, Provider, Error));
	TestFalse(TEXT("An already-created snapshot observes withdrawal"),
				   Snapshot->ResolveReadyExact(SelectionA, Selected, SelectionError));
	TestEqual(TEXT("Stale snapshot reports typed configuration failure"), SelectionError.Code,
				   EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
	TestEqual(TEXT("Stale snapshot no longer lists a withdrawn choice"), Snapshot->List().Num(), 1);

	const TSharedRef<FCatalogTestProvider, ESPMode::ThreadSafe> DifferentProvider =
		MakeShared<FCatalogTestProvider, ESPMode::ThreadSafe>();
	TestFalse(TEXT("Different instance cannot withdraw account B"),
				   Catalog.Unregister(SelectionB, DifferentProvider, Error));
	TestTrue(TEXT("Exact provider withdraws account B"), Catalog.Unregister(SelectionB, Provider, Error));
	TestEqual(TEXT("Catalog is empty after exact withdrawal"), Catalog.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
