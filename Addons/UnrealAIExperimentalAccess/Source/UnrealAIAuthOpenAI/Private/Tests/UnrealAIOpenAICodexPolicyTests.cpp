// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "OpenAI/UnrealAIOpenAICodexProviderPolicy.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIOpenAICodexPolicyTest, "UnrealAI.OpenAI.CodexResponsesWirePolicy",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIOpenAICodexPolicyTest::RunTest(const FString &)
{
	FUnrealAIOpenAIResponsesProviderConfig Config;
	FString Error;
	if (!TestTrue(TEXT("Subscription policy is valid"),
					   UE::UnrealAI::OpenAICodex::TryBuildResponsesPolicy(Config, Error)))
	{
		return false;
	}
	TestFalse(TEXT("Subscription endpoint omits unsupported output-token field"), Config.bSendMaxOutputTokens);
	TestTrue(TEXT("Subscription endpoint explicitly permits an absent media header"), Config.bAllowMissingResponseContentType);
	TestTrue(TEXT("Subscription endpoint explicitly permits metadata-only completion"), Config.bAllowEmptyTerminalOutput);
	TestFalse(TEXT("Public API retains terminal output reconciliation"),
		FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey().bAllowEmptyTerminalOutput);
	TestFalse(TEXT("Subscription endpoint uses Responses"), Config.bUseChatCompletions);
	TestEqual(TEXT("Subscription profile uses an exact supported model slug"), Config.ModelProfiles[0].ModelId,
				   FString(TEXT("gpt-5.5")));
	TestEqual(TEXT("Subscription billing remains explicit"), Config.BillingMode,
				   EUnrealAIBillingMode::SubscriptionQuota);
	TestTrue(TEXT("Public API token limit remains enabled"),
				  FUnrealAIOpenAIResponsesProviderConfig::OpenAIPlatformApiKey().bSendMaxOutputTokens);
	return true;
}
#endif
