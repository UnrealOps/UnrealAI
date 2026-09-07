// Copyright EngineWorks. All Rights Reserved.

#include "Models/UnrealAIProviderCatalog.h"
#include "Transport/UnrealAIHttpTransport.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIBuiltInProviderSetupTest, "UnrealAI.Auth.BuiltInProviderConfiguration",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIBuiltInProviderSetupTest::RunTest(const FString &Parameters)
{
	if (!IsUnrealAIPlatformHttpsTransportSupported())
	{
		AddInfo(TEXT("Native platform transport is unavailable; built-in secure-store routes remain unavailable."));
		return true;
	}
	struct FExpectedRoute
	{
		FName Provider;
		const TCHAR *Audience;
		FUnrealAIAccessAccountId Account;
	};
	const FExpectedRoute Routes[] = {
		{TEXT("openai.responses"),
			  TEXT("https://api.openai.com/v1/responses"), {FGuid(0x12614b23, 0x71d54d80, 0xa1112d16, 0x9daf3ef0)}},
		 {TEXT("anthropic.messages"),
			   TEXT("https://api.anthropic.com/v1/messages"), {FGuid(0x12101001, 0x12101002, 0x12101003, 0x12101004)}},
		  {TEXT("gemini.interactions"),
				TEXT("https://generativelanguage.googleapis.com/v1/interactions"),
					 {FGuid(0x12201001, 0x12201002, 0x12201003, 0x12201004)}},
		   {TEXT("xai.platform.responses"), TEXT("https://api.x.ai/v1/responses"),
												 {FGuid(0x12381001, 0x12381002, 0x12381003, 0x12381004)}}};
	for (const FExpectedRoute &Route : Routes)
	{
		const auto Registration = FUnrealAIProviderCatalog::Get().Find(Route.Provider);
		if (!TestTrue(*Route.Provider.ToString(), Registration.IsValid()))
		{
			continue;
		}
		FString Error;
		TestTrue(TEXT("Catalog entry validates"), Registration->Validate(Error));
		const auto Connection = Registration->Connections->Find(Registration->DefaultConnectionAlias);
		if (!TestTrue(TEXT("Exact connection is present"), Connection.IsValid()))
		{
			continue;
		}
		TestEqual(TEXT("SDK preserves the original API resource"), Connection->CredentialDestination.Audience,
					   FString(Route.Audience));
		TestTrue(TEXT("SDK preserves the account identity used by existing stored credentials"),
					  Connection->CredentialDestination.AccountId == Route.Account);
		TestTrue(TEXT("Provisioning is supplied by the optional auth plugin"),
					  Registration->ApiKeyProvisioner.IsValid());
	}
	return true;
}
#endif
