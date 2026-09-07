// Copyright UnrealOps. All Rights Reserved.

#include "Auth/UnrealAIPlatformSecretStore.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
class FPlatformSelectionTestStore final : public IUnrealAISecretStore
{
  public:
	FPlatformSelectionTestStore(const FName InStoreName, const FUnrealAISecretStoreCapabilities &InCapabilities)
		: StoreName(InStoreName), Capabilities(InCapabilities)
	{
	}

	FName GetStoreName() const override
	{
		return StoreName;
	}

	FUnrealAISecretStoreCapabilities DescribeCapabilities() const override
	{
		return Capabilities;
	}

	EUnrealAISecretStoreResult Load(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									FUnrealAISecretValue &OutValue, uint64 &OutRevision,
									FUnrealAIProviderAccessError &OutError) override
	{
		OutValue.Reset();
		OutRevision = 0;
		return NotSupported(OutError);
	}

	EUnrealAISecretStoreResult Store(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									 const FUnrealAISecretValue &, uint64, uint64 &OutNewRevision,
									 FUnrealAIProviderAccessError &OutError) override
	{
		OutNewRevision = 0;
		return NotSupported(OutError);
	}

	EUnrealAISecretStoreResult Delete(const FUnrealAISecretStoreOperationContext &, const FUnrealAISecretHandle &,
									  uint64, FUnrealAIProviderAccessError &OutError) override
	{
		return NotSupported(OutError);
	}

	void SetCapabilities(const FUnrealAISecretStoreCapabilities &InCapabilities)
	{
		Capabilities = InCapabilities;
	}

  private:
	static EUnrealAISecretStoreResult NotSupported(FUnrealAIProviderAccessError &OutError)
	{
		OutError = {};
		OutError.Category = EUnrealAIErrorCategory::UnsupportedCapability;
		OutError.Code = EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported;
		return EUnrealAISecretStoreResult::NotSupported;
	}

	FName StoreName;
	FUnrealAISecretStoreCapabilities Capabilities;
};

FUnrealAISecretStoreCapabilities MakePlatformCapabilities(const EUnrealAISecretStoreProtectionClass Protection,
														  const EUnrealAISecretStoreScopeClass Scope,
														  const bool bAtomicCompareAndSwap,
														  const bool bAvailableInShipping)
{
	FUnrealAISecretStoreCapabilities Capabilities;
	Capabilities.PersistenceClass = EUnrealAISecretStorePersistenceClass::Persistent;
	Capabilities.ProtectionClass = Protection;
	Capabilities.ScopeClass = Scope;
	Capabilities.bAvailableInCurrentBuild = true;
	Capabilities.bPersistent = true;
	Capabilities.bAtomicCompareAndSwap = bAtomicCompareAndSwap;
	Capabilities.bAvailableInShipping = bAvailableInShipping;
	return Capabilities;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIPlatformSecretStoreSelectionTest,
								 "UnrealAI.Auth.PlatformSecretStore.SelectionFailsClosed",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIPlatformSecretStoreSelectionTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	const FUnrealAIPlatformSecretStoreSelection ApiKey =
		FUnrealAIPlatformSecretStoreSelection::CurrentUserApiKey(false);
	const FUnrealAIPlatformSecretStoreSelection OAuth = FUnrealAIPlatformSecretStoreSelection::CurrentUserOAuth(false);
	const FUnrealAIPlatformSecretStoreSelection Service =
		FUnrealAIPlatformSecretStoreSelection::AuthoritativeService(true);
	FString Error;
	TestTrue(TEXT("API-key selection is valid"), ApiKey.ValidateShape(Error));
	TestTrue(TEXT("OAuth selection is valid"), OAuth.ValidateShape(Error));
	TestTrue(TEXT("authoritative selection is valid"), Service.ValidateShape(Error));
	TestFalse(TEXT("API-key selection does not require CAS"), ApiKey.bRequireAtomicCompareAndSwap);
	TestTrue(TEXT("OAuth selection requires CAS"), OAuth.bRequireAtomicCompareAndSwap);
	TestTrue(TEXT("service selection is Shipping-bound"), Service.bRequireAvailableInShipping);

	TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> Store;
	FUnrealAISecretStoreCapabilities Capabilities;
	FUnrealAIProviderAccessError AccessError;
	TestFalse(TEXT("built-in current-user stores never satisfy authoritative service scope"),
				   FUnrealAIPlatformSecretStore::TryCreate(Service, Store, Capabilities, AccessError));
	TestFalse(TEXT("failed service selection returns no store"), Store.IsValid());
	TestFalse(TEXT("failed service selection advertises no capabilities"), Capabilities.bAvailableInCurrentBuild);
	TestEqual(TEXT("service selection returns typed unsupported"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);

	Store.Reset();
	Capabilities = {};
	AccessError = {};
#if PLATFORM_MAC
	TestTrue(TEXT("Mac current-user OAuth selects Keychain"),
				  FUnrealAIPlatformSecretStore::TryCreate(OAuth, Store, Capabilities, AccessError));
	TestTrue(TEXT("selected Mac store is valid"), Store.IsValid());
	if (Store.IsValid())
	{
		TestEqual(TEXT("factory name matches selected store"), Store->GetStoreName(),
					   FUnrealAIPlatformSecretStore::GetCompiledStoreName());
	}
	TestTrue(TEXT("selected Mac store is production protected"), Capabilities.IsProductionProtected());
	TestTrue(TEXT("selected Mac store supports CAS"), Capabilities.bAtomicCompareAndSwap);
#else
	TestFalse(TEXT("unsupported platform fails closed"),
				   FUnrealAIPlatformSecretStore::TryCreate(OAuth, Store, Capabilities, AccessError));
	TestTrue(TEXT("unsupported build has no compiled store name"),
				  FUnrealAIPlatformSecretStore::GetCompiledStoreName().IsNone());
	TestEqual(TEXT("unsupported platform is typed"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIPlatformSecretStoreAdapterRegistrationTest,
								 "UnrealAI.Auth.PlatformSecretStore.ExactAdapterScopeShippingAndDrift",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIPlatformSecretStoreAdapterRegistrationTest::RunTest(const FString &Parameters)
{
	(void)Parameters;
	FString Error;
	const FUnrealAISecretStoreCapabilities ProcessCapabilities = []
	{
		FUnrealAISecretStoreCapabilities Value;
		Value.PersistenceClass = EUnrealAISecretStorePersistenceClass::Volatile;
		Value.ProtectionClass = EUnrealAISecretStoreProtectionClass::ProcessMemory;
		Value.ScopeClass = EUnrealAISecretStoreScopeClass::Process;
		Value.bAvailableInCurrentBuild = true;
		return Value;
	}();
	const TSharedRef<FPlatformSelectionTestStore, ESPMode::ThreadSafe> ProcessStore =
		MakeShared<FPlatformSelectionTestStore, ESPMode::ThreadSafe>(TEXT("tests.platform.process"),
																		  ProcessCapabilities);
	TestFalse(TEXT("process memory is never a production adapter"),
				   FUnrealAIPlatformSecretStore::RegisterAdapterForAutomation(
					   EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser, ProcessStore, Error));

	const FUnrealAISecretStoreCapabilities LocalMachineCapabilities =
		MakePlatformCapabilities(EUnrealAISecretStoreProtectionClass::PlatformCredentialStore,
								 EUnrealAISecretStoreScopeClass::LocalMachine, true, true);
	const TSharedRef<FPlatformSelectionTestStore, ESPMode::ThreadSafe> LocalMachineStore =
		MakeShared<FPlatformSelectionTestStore, ESPMode::ThreadSafe>(TEXT("tests.platform.local_machine"),
																		  LocalMachineCapabilities);
	TestFalse(TEXT("interactive credentials cannot widen to local-machine scope"),
				   FUnrealAIPlatformSecretStore::RegisterAdapterForAutomation(
					   EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser, LocalMachineStore, Error));

	const FUnrealAISecretStoreCapabilities CurrentUserCapabilities =
		MakePlatformCapabilities(EUnrealAISecretStoreProtectionClass::PlatformCredentialStore,
								 EUnrealAISecretStoreScopeClass::CurrentUser, true, true);
	const TSharedRef<FPlatformSelectionTestStore, ESPMode::ThreadSafe> CurrentUserStore =
		MakeShared<FPlatformSelectionTestStore, ESPMode::ThreadSafe>(TEXT("tests.platform.current_user"),
																		  CurrentUserCapabilities);
	const TSharedRef<FPlatformSelectionTestStore, ESPMode::ThreadSafe> DifferentCurrentUserStore =
		MakeShared<FPlatformSelectionTestStore, ESPMode::ThreadSafe>(TEXT("tests.platform.current_user.other"),
																		  CurrentUserCapabilities);
	TestTrue(TEXT("reviewed current-user adapter registers"),
				  FUnrealAIPlatformSecretStore::RegisterAdapterForAutomation(
					  EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser, CurrentUserStore, Error));
	TestFalse(TEXT("one purpose cannot become ambiguous"),
				   FUnrealAIPlatformSecretStore::RegisterAdapterForAutomation(
					   EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser, DifferentCurrentUserStore, Error));

	const FUnrealAIPlatformSecretStoreSelection OAuthSelection =
		FUnrealAIPlatformSecretStoreSelection::CurrentUserOAuth(true);
	TestEqual(TEXT("exact registered store wins deterministic selection"),
				   FUnrealAIPlatformSecretStore::GetSelectedStoreName(OAuthSelection),
				   CurrentUserStore->GetStoreName());
	TSharedPtr<IUnrealAISecretStore, ESPMode::ThreadSafe> SelectedStore;
	FUnrealAISecretStoreCapabilities SelectedCapabilities;
	FUnrealAIProviderAccessError AccessError;
	TestTrue(
		TEXT("OAuth selection returns the registered current-user adapter"),
			 FUnrealAIPlatformSecretStore::TryCreate(OAuthSelection, SelectedStore, SelectedCapabilities, AccessError));
	TestTrue(TEXT("selection preserves the exact adapter instance"),
				  SelectedStore.IsValid() && SelectedStore.Get() == &CurrentUserStore.Get());

	FUnrealAISecretStoreCapabilities DriftedCapabilities = CurrentUserCapabilities;
	DriftedCapabilities.bAtomicCompareAndSwap = false;
	CurrentUserStore->SetCapabilities(DriftedCapabilities);
	SelectedStore.Reset();
	TestFalse(
		TEXT("post-registration capability drift fails closed"),
			 FUnrealAIPlatformSecretStore::TryCreate(OAuthSelection, SelectedStore, SelectedCapabilities, AccessError));
	TestEqual(TEXT("capability drift has a typed configuration error"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::InvalidConfiguration);
	CurrentUserStore->SetCapabilities(CurrentUserCapabilities);

	TestFalse(TEXT("a different instance cannot withdraw the adapter"),
				   FUnrealAIPlatformSecretStore::UnregisterAdapter(
					   EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser, DifferentCurrentUserStore, Error));
	TestTrue(TEXT("the exact current-user adapter withdraws"),
				  FUnrealAIPlatformSecretStore::UnregisterAdapter(
					  EUnrealAIPlatformSecretStorePurpose::InteractiveCurrentUser, CurrentUserStore, Error));

	const FUnrealAISecretStoreCapabilities WrongServiceCapabilities =
		MakePlatformCapabilities(EUnrealAISecretStoreProtectionClass::PlatformCredentialStore,
								 EUnrealAISecretStoreScopeClass::CurrentUser, true, true);
	const TSharedRef<FPlatformSelectionTestStore, ESPMode::ThreadSafe> WrongServiceStore =
		MakeShared<FPlatformSelectionTestStore, ESPMode::ThreadSafe>(TEXT("tests.platform.wrong_service"),
																		  WrongServiceCapabilities);
	TestFalse(TEXT("authoritative service cannot use a user credential store"),
				   FUnrealAIPlatformSecretStore::RegisterAdapterForAutomation(
					   EUnrealAIPlatformSecretStorePurpose::AuthoritativeService, WrongServiceStore, Error));

	const FUnrealAISecretStoreCapabilities NonShippingServiceCapabilities =
		MakePlatformCapabilities(EUnrealAISecretStoreProtectionClass::ExternalSecretService,
								 EUnrealAISecretStoreScopeClass::Service, true, false);
	const TSharedRef<FPlatformSelectionTestStore, ESPMode::ThreadSafe> NonShippingServiceStore =
		MakeShared<FPlatformSelectionTestStore, ESPMode::ThreadSafe>(TEXT("tests.platform.service.development"),
																		  NonShippingServiceCapabilities);
	TestTrue(TEXT("development service adapter registers with truthful capabilities"),
				  FUnrealAIPlatformSecretStore::RegisterAdapterForAutomation(
					  EUnrealAIPlatformSecretStorePurpose::AuthoritativeService, NonShippingServiceStore, Error));
	TestFalse(
		TEXT("Shipping selection rejects a development-only service adapter"),
			 FUnrealAIPlatformSecretStore::TryCreate(FUnrealAIPlatformSecretStoreSelection::AuthoritativeService(true),
													 SelectedStore, SelectedCapabilities, AccessError));
	TestEqual(TEXT("Shipping rejection is typed unsupported"), AccessError.Code,
				   EUnrealAIProviderAccessErrorCode::SecretStoreNotSupported);
	TestTrue(TEXT("development service adapter withdraws exactly"),
				  FUnrealAIPlatformSecretStore::UnregisterAdapter(
					  EUnrealAIPlatformSecretStorePurpose::AuthoritativeService, NonShippingServiceStore, Error));

	const FUnrealAISecretStoreCapabilities ServiceCapabilities =
		MakePlatformCapabilities(EUnrealAISecretStoreProtectionClass::ExternalSecretService,
								 EUnrealAISecretStoreScopeClass::Service, true, true);
	const TSharedRef<FPlatformSelectionTestStore, ESPMode::ThreadSafe> ServiceStore =
		MakeShared<FPlatformSelectionTestStore, ESPMode::ThreadSafe>(TEXT("tests.platform.service.shipping"),
																		  ServiceCapabilities);
	TestTrue(TEXT("Shipping-capable external service adapter registers"),
				  FUnrealAIPlatformSecretStore::RegisterAdapterForAutomation(
					  EUnrealAIPlatformSecretStorePurpose::AuthoritativeService, ServiceStore, Error));
	SelectedStore.Reset();
	TestTrue(
		TEXT("authoritative Shipping selection resolves exact external service"),
			 FUnrealAIPlatformSecretStore::TryCreate(FUnrealAIPlatformSecretStoreSelection::AuthoritativeService(true),
													 SelectedStore, SelectedCapabilities, AccessError));
	TestTrue(TEXT("service selection preserves the exact adapter instance"),
				  SelectedStore.IsValid() && SelectedStore.Get() == &ServiceStore.Get());
	TestTrue(TEXT("exact service adapter withdraws"),
				  FUnrealAIPlatformSecretStore::UnregisterAdapter(
					  EUnrealAIPlatformSecretStorePurpose::AuthoritativeService, ServiceStore, Error));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
