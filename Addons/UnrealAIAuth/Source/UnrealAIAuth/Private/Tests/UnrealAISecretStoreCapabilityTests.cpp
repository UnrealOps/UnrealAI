// Copyright EngineWorks. All Rights Reserved.
#include "Auth/UnrealAIMacKeychainSecretStore.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIPlatformCapabilityTest, "UnrealAI.Auth.SecretStoreCapabilityMetadata",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIPlatformCapabilityTest::RunTest(const FString &Parameters)
{
	FString Error;
	const FUnrealAIMacKeychainSecretStore MacKeychain;
	const FUnrealAISecretStoreCapabilities Mac = MacKeychain.DescribeCapabilities();
	TestTrue(TEXT("Keychain descriptor is internally consistent on every target"), Mac.ValidateShape(Error));
#if PLATFORM_MAC
	TestTrue(TEXT("macOS Keychain is available in the macOS build"), Mac.bAvailableInCurrentBuild);
	TestEqual(TEXT("macOS Keychain is persistent"), Mac.PersistenceClass,
				   EUnrealAISecretStorePersistenceClass::Persistent);
	TestEqual(TEXT("macOS Keychain advertises only the platform credential-store class"), Mac.ProtectionClass,
				   EUnrealAISecretStoreProtectionClass::PlatformCredentialStore);
	TestEqual(TEXT("macOS Keychain uses current-user scope"), Mac.ScopeClass,
				   EUnrealAISecretStoreScopeClass::CurrentUser);
	TestTrue(TEXT("macOS Keychain is production protected"), Mac.IsProductionProtected());
#else
	TestFalse(TEXT("The macOS implementation does not over-advertise another target"), Mac.bAvailableInCurrentBuild);
	TestEqual(TEXT("Unsupported targets retain invalid persistence metadata"), Mac.PersistenceClass,
				   EUnrealAISecretStorePersistenceClass::Invalid);
	TestFalse(TEXT("Unsupported targets are not production protected"), Mac.IsProductionProtected());
#endif
	return true;
}
#endif
