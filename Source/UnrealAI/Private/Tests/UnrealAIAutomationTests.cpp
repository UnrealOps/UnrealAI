#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIChatCompletionAsyncAction.h"
#include "UnrealAIChatComponent.h"
#include "UnrealAISettings.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIBlueprintHelpersTest,
	"UnrealAI.Blueprint.Helpers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBlueprintHelpersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FUnrealAIChatRequest Request = UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(TEXT("Hello"), TEXT("test-model"));
	TestEqual(TEXT("The requested model is preserved"), Request.Model, FString(TEXT("test-model")));
	TestEqual(TEXT("A simple request contains one message"), Request.Messages.Num(), 1);
	if (Request.Messages.Num() == 1)
	{
		TestEqual(TEXT("The message uses the user role"), Request.Messages[0].Role, EUnrealAIMessageRole::User);
		TestEqual(TEXT("The prompt is preserved"), Request.Messages[0].Content, FString(TEXT("Hello")));
	}

	TestEqual(
		TEXT("The JSON object response format is stable"),
		UUnrealAIBlueprintLibrary::MakeJsonObjectResponseFormat(),
		FString(TEXT("{\"type\":\"json_object\"}")));
	TestTrue(
		TEXT("A valid strict schema produces a JSON schema response format"),
		UUnrealAIBlueprintLibrary::MakeStrictJsonSchemaResponseFormat(
			TEXT("player_state"), TEXT("{\"type\":\"object\"}"), true)
			.Contains(TEXT("\"type\":\"json_schema\"")));
	TestTrue(
		TEXT("An invalid schema is rejected"),
		UUnrealAIBlueprintLibrary::MakeStrictJsonSchemaResponseFormat(TEXT("invalid"), TEXT("{"), true).IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIDefaultProvidersTest,
	"UnrealAI.Settings.DefaultProviders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIDefaultProvidersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const UUnrealAISettings* Settings = NewObject<UUnrealAISettings>();
	TestNotNull(TEXT("Default settings can be constructed"), Settings);
	if (!Settings)
	{
		return false;
	}

	const FUnrealAIProviderConfig* OpenAIProvider = Settings->ProviderProfiles.FindByPredicate(
		[](const FUnrealAIProviderConfig& Provider)
		{
			return Provider.Name == TEXT("OpenAI");
		});
	TestNotNull(TEXT("The OpenAI profile exists"), OpenAIProvider);
	if (OpenAIProvider)
	{
		TestEqual(TEXT("The OpenAI base URL is correct"), OpenAIProvider->BaseUrl, FString(TEXT("https://api.openai.com/v1")));
		TestEqual(TEXT("The OpenAI model is correct"), OpenAIProvider->DefaultModel, FString(TEXT("gpt-5.6-luna")));
		TestEqual(TEXT("The OpenAI API key environment variable is correct"), OpenAIProvider->ApiKeyEnvironmentVariable, FString(TEXT("OPENAI_API_KEY")));
		TestTrue(TEXT("The OpenAI profile contains no API key override"), OpenAIProvider->ApiKeyOverride.IsEmpty());
	}

	const FUnrealAIProviderConfig* XAIProvider = Settings->ProviderProfiles.FindByPredicate(
		[](const FUnrealAIProviderConfig& Provider)
		{
			return Provider.Name == TEXT("XAI");
		});
	TestNotNull(TEXT("The xAI profile exists"), XAIProvider);
	if (XAIProvider)
	{
		TestEqual(TEXT("The xAI base URL is correct"), XAIProvider->BaseUrl, FString(TEXT("https://api.x.ai/v1")));
		TestEqual(TEXT("The xAI model is correct"), XAIProvider->DefaultModel, FString(TEXT("grok-4.6")));
		TestEqual(TEXT("The xAI API key environment variable is correct"), XAIProvider->ApiKeyEnvironmentVariable, FString(TEXT("XAI_API_KEY")));
		TestTrue(TEXT("The xAI profile contains no API key override"), XAIProvider->ApiKeyOverride.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIResponseHelpersTest,
	"UnrealAI.Response.FirstChoice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIResponseHelpersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FUnrealAIChatResponse EmptyResponse;
	bool bHasContent = true;
	TestTrue(
		TEXT("An empty response returns an empty string"),
		UUnrealAIBlueprintLibrary::GetFirstChoiceContent(EmptyResponse, bHasContent).IsEmpty());
	TestFalse(TEXT("An empty response reports no content"), bHasContent);

	FUnrealAIChatChoice Choice;
	Choice.Content = TEXT("Assistant response");
	EmptyResponse.Choices.Add(Choice);
	TestEqual(
		TEXT("The first choice content is returned"),
		UUnrealAIBlueprintLibrary::GetFirstChoiceContent(EmptyResponse, bHasContent),
		FString(TEXT("Assistant response")));
	TestTrue(TEXT("A populated response reports content"), bHasContent);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIBlueprintSurfaceTest,
	"UnrealAI.Blueprint.Surface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIBlueprintSurfaceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const UFunction* MakeRequest = UUnrealAIBlueprintLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIBlueprintLibrary, MakeSimpleChatRequest));
	TestNotNull(TEXT("Make Simple Chat Request is reflected"), MakeRequest);
	if (MakeRequest)
	{
		TestTrue(TEXT("Make Simple Chat Request is Blueprint callable"), MakeRequest->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		TestTrue(TEXT("Make Simple Chat Request is Blueprint pure"), MakeRequest->HasAnyFunctionFlags(FUNC_BlueprintPure));
	}

	const UFunction* FirstContent = UUnrealAIBlueprintLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIBlueprintLibrary, GetFirstChoiceContent));
	TestNotNull(TEXT("Get First Choice Content is reflected"), FirstContent);
	if (FirstContent)
	{
		TestTrue(TEXT("Get First Choice Content is Blueprint callable"), FirstContent->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		TestTrue(TEXT("Get First Choice Content is Blueprint pure"), FirstContent->HasAnyFunctionFlags(FUNC_BlueprintPure));
	}

	const UFunction* AsyncRequest = UUnrealAIChatCompletionAsyncAction::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatCompletionAsyncAction, CreateChatCompletion));
	TestNotNull(TEXT("Create Chat Completion is reflected"), AsyncRequest);
	if (AsyncRequest)
	{
		TestTrue(TEXT("Create Chat Completion is Blueprint callable"), AsyncRequest->HasAnyFunctionFlags(FUNC_BlueprintCallable));
#if WITH_METADATA
		TestEqual(
			TEXT("Create Chat Completion has the documented display name"),
			AsyncRequest->GetMetaData(TEXT("DisplayName")),
			FString(TEXT("Create Chat Completion (UnrealAI)")));
#endif
	}

	const UFunction* SendPrompt = UUnrealAIChatComponent::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatComponent, SendPrompt));
	TestNotNull(TEXT("Send Prompt is reflected"), SendPrompt);
	if (SendPrompt)
	{
		TestTrue(TEXT("Send Prompt is Blueprint callable"), SendPrompt->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	const FMulticastDelegateProperty* Completed = FindFProperty<FMulticastDelegateProperty>(
		UUnrealAIChatComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatCompleted));
	TestNotNull(TEXT("On Chat Completed is reflected"), Completed);
	if (Completed)
	{
		TestTrue(TEXT("On Chat Completed is Blueprint assignable"), Completed->HasAnyPropertyFlags(CPF_BlueprintAssignable));
	}

	const FMulticastDelegateProperty* Failed = FindFProperty<FMulticastDelegateProperty>(
		UUnrealAIChatComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatFailed));
	TestNotNull(TEXT("On Chat Failed is reflected"), Failed);
	if (Failed)
	{
		TestTrue(TEXT("On Chat Failed is Blueprint assignable"), Failed->HasAnyPropertyFlags(CPF_BlueprintAssignable));
	}

	return true;
}

#endif
