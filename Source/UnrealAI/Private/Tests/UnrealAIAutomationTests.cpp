#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIChatCompletionAsyncAction.h"
#include "UnrealAIChatComponent.h"
#include "UnrealAIClient.h"
#include "UnrealAIProviderAdapter.h"
#include "UnrealAIProviders.h"
#include "UnrealAISettings.h"
#include "UObject/UnrealType.h"

namespace UnrealAIAutomationTestsPrivate
{
	bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}
}

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
		TestEqual(TEXT("The OpenAI protocol is correct"), OpenAIProvider->Api, EUnrealAIProviderApi::OpenAICompatibleChatCompletions);
		TestEqual(TEXT("The OpenAI base URL is correct"), OpenAIProvider->BaseUrl, FString(TEXT("https://api.openai.com/v1")));
		TestEqual(TEXT("The OpenAI base URL environment variable is correct"), OpenAIProvider->BaseUrlEnvironmentVariable, FString(TEXT("OPENAI_BASE_URL")));
		TestEqual(TEXT("The OpenAI model is correct"), OpenAIProvider->DefaultModel, FString(TEXT("gpt-5.6-luna")));
		TestEqual(TEXT("The OpenAI model environment variable is correct"), OpenAIProvider->ModelEnvironmentVariable, FString(TEXT("OPENAI_MODEL")));
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
		TestEqual(TEXT("The xAI protocol is correct"), XAIProvider->Api, EUnrealAIProviderApi::OpenAICompatibleChatCompletions);
		TestEqual(TEXT("The xAI base URL is correct"), XAIProvider->BaseUrl, FString(TEXT("https://api.x.ai/v1")));
		TestEqual(TEXT("The xAI base URL environment variable is correct"), XAIProvider->BaseUrlEnvironmentVariable, FString(TEXT("XAI_BASE_URL")));
		TestEqual(TEXT("The xAI model is correct"), XAIProvider->DefaultModel, FString(TEXT("grok-4.6")));
		TestEqual(TEXT("The xAI model environment variable is correct"), XAIProvider->ModelEnvironmentVariable, FString(TEXT("XAI_MODEL")));
		TestEqual(TEXT("The xAI API key environment variable is correct"), XAIProvider->ApiKeyEnvironmentVariable, FString(TEXT("XAI_API_KEY")));
		TestTrue(TEXT("The xAI profile contains no API key override"), XAIProvider->ApiKeyOverride.IsEmpty());
	}

	const FUnrealAIProviderConfig* AnthropicProvider = Settings->ProviderProfiles.FindByPredicate(
		[](const FUnrealAIProviderConfig& Provider)
		{
			return Provider.Name == TEXT("Anthropic");
		});
	TestNotNull(TEXT("The Anthropic profile exists"), AnthropicProvider);
	if (AnthropicProvider)
	{
		TestEqual(TEXT("The Anthropic protocol is correct"), AnthropicProvider->Api, EUnrealAIProviderApi::AnthropicMessages);
		TestEqual(TEXT("The Anthropic base URL is correct"), AnthropicProvider->BaseUrl, FString(TEXT("https://api.anthropic.com/v1")));
		TestEqual(TEXT("The Anthropic model is correct"), AnthropicProvider->DefaultModel, FString(TEXT("claude-sonnet-5")));
		TestEqual(TEXT("The Anthropic API key environment variable is correct"), AnthropicProvider->ApiKeyEnvironmentVariable, FString(TEXT("ANTHROPIC_API_KEY")));
		TestTrue(TEXT("The Anthropic profile contains no API key override"), AnthropicProvider->ApiKeyOverride.IsEmpty());
	}

	const FUnrealAIProviderConfig* GeminiProvider = Settings->ProviderProfiles.FindByPredicate(
		[](const FUnrealAIProviderConfig& Provider)
		{
			return Provider.Name == TEXT("Gemini");
		});
	TestNotNull(TEXT("The Gemini profile exists"), GeminiProvider);
	if (GeminiProvider)
	{
		TestEqual(TEXT("The Gemini protocol is correct"), GeminiProvider->Api, EUnrealAIProviderApi::GeminiGenerateContent);
		TestEqual(TEXT("The Gemini base URL is correct"), GeminiProvider->BaseUrl, FString(TEXT("https://generativelanguage.googleapis.com/v1beta")));
		TestEqual(TEXT("The Gemini model is correct"), GeminiProvider->DefaultModel, FString(TEXT("gemini-3.7-flash")));
		TestEqual(TEXT("The Gemini API key environment variable is correct"), GeminiProvider->ApiKeyEnvironmentVariable, FString(TEXT("GEMINI_API_KEY")));
		TestTrue(TEXT("The Gemini profile contains no API key override"), GeminiProvider->ApiKeyOverride.IsEmpty());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIProviderFactoriesTest,
	"UnrealAI.Providers.Factories",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIProviderFactoriesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UObject* Outer = NewObject<UUnrealAISettings>();
	FUnrealAIProviderConfig CustomConfig;
	CustomConfig.Name = TEXT("LocalGateway");
	CustomConfig.Api = EUnrealAIProviderApi::AnthropicMessages;
	CustomConfig.BaseUrl = TEXT("http://127.0.0.1:8080/v1");
	CustomConfig.DefaultModel = TEXT("local-model");
	CustomConfig.bRequiresApiKey = false;

	FUnrealAIError Error;
	UUnrealAIClient* Client = UUnrealAIProviders::OpenAICompatible(
		Outer,
		CustomConfig,
		Error,
		TEXT("model-override"));
	TestNotNull(TEXT("A custom OpenAI-compatible factory creates a client"), Client);
	TestFalse(TEXT("A successful factory call clears the error"), Error.bIsError);
	if (Client)
	{
		TestTrue(TEXT("The factory configures the client"), Client->IsConfigured());
		TestEqual(
			TEXT("The compatible factory selects the OpenAI-compatible protocol"),
			Client->GetProviderConfig().Api,
			EUnrealAIProviderApi::OpenAICompatibleChatCompletions);
		TestEqual(
			TEXT("The factory model override is applied"),
			Client->GetProviderConfig().DefaultModel,
			FString(TEXT("model-override")));
	}

	FUnrealAIError InvalidOuterError;
	TestNull(
		TEXT("A factory rejects a missing UObject outer"),
		UUnrealAIProviders::Gemini(nullptr, InvalidOuterError));
	TestTrue(TEXT("A missing outer produces an error"), InvalidOuterError.bIsError);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIProviderAdaptersTest,
	"UnrealAI.Providers.ProtocolAdapters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIProviderAdaptersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FUnrealAIChatRequest Request;
	FUnrealAIChatMessage SystemMessage;
	SystemMessage.Role = EUnrealAIMessageRole::System;
	SystemMessage.Content = TEXT("Be concise.");
	Request.Messages.Add(SystemMessage);
	FUnrealAIChatMessage UserMessage;
	UserMessage.Role = EUnrealAIMessageRole::User;
	UserMessage.Content = TEXT("Hello");
	Request.Messages.Add(UserMessage);
	Request.bUseTemperature = true;
	Request.Temperature = 0.25f;
	Request.bUseMaxCompletionTokens = true;
	Request.MaxCompletionTokens = 64;
	Request.StopSequences.Add(TEXT("STOP"));

	FUnrealAIChatRequest OpenAIRequest = Request;
	OpenAIRequest.NumChoices = 2;
	OpenAIRequest.ResponseFormatJson = TEXT("{\"type\":\"json_object\"}");
	OpenAIRequest.AdditionalParametersJson = TEXT("{\"seed\":42}");
	FUnrealAIProviderConfig OpenAIConfig;
	OpenAIConfig.Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
	OpenAIConfig.BaseUrl = TEXT("https://compatible.example/v1/");
	OpenAIConfig.DefaultModel = TEXT("compatible-test");
	OpenAIConfig.OrganizationId = TEXT("org-test");
	OpenAIConfig.ProjectId = TEXT("project-test");
	FUnrealAIHttpRequestData OpenAIHttpRequest;
	FUnrealAIError OpenAIBuildError;
	TestTrue(
		TEXT("OpenAI-compatible request construction succeeds"),
		UnrealAIProviderAdapters::Get(OpenAIConfig.Api).BuildRequest(
			OpenAIConfig,
			OpenAIRequest,
			TEXT("openai-test-key"),
			OpenAIHttpRequest,
			OpenAIBuildError));
	TestFalse(TEXT("OpenAI-compatible request construction has no error"), OpenAIBuildError.bIsError);
	TestEqual(
		TEXT("OpenAI-compatible profiles use the Chat Completions endpoint"),
		OpenAIHttpRequest.Url,
		FString(TEXT("https://compatible.example/v1/chat/completions")));
	TestEqual(TEXT("OpenAI-compatible authentication uses a bearer token"), OpenAIHttpRequest.Headers.FindRef(TEXT("Authorization")), FString(TEXT("Bearer openai-test-key")));
	TestEqual(TEXT("OpenAI organization headers are preserved"), OpenAIHttpRequest.Headers.FindRef(TEXT("OpenAI-Organization")), FString(TEXT("org-test")));
	TestEqual(TEXT("OpenAI project headers are preserved"), OpenAIHttpRequest.Headers.FindRef(TEXT("OpenAI-Project")), FString(TEXT("project-test")));
	TSharedPtr<FJsonObject> OpenAIPayload;
	TestTrue(
		TEXT("OpenAI-compatible request serializes a valid JSON object"),
		UnrealAIAutomationTestsPrivate::ParseJsonObject(OpenAIHttpRequest.Body, OpenAIPayload));
	if (OpenAIPayload.IsValid())
	{
		int32 NumChoices = 0;
		int32 Seed = 0;
		TestTrue(TEXT("OpenAI-compatible request includes n"), OpenAIPayload->TryGetNumberField(TEXT("n"), NumChoices));
		TestEqual(TEXT("OpenAI-compatible request maps choice count"), NumChoices, 2);
		TestTrue(TEXT("OpenAI-compatible request merges additional parameters"), OpenAIPayload->TryGetNumberField(TEXT("seed"), Seed));
		TestEqual(TEXT("OpenAI-compatible additional parameters preserve values"), Seed, 42);
		TestTrue(TEXT("OpenAI-compatible request includes response_format"), OpenAIPayload->HasTypedField<EJson::Object>(TEXT("response_format")));
	}

	const FString OpenAIResponseJson =
		TEXT("{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion\",\"created\":123,\"model\":\"compatible-test\",")
		TEXT("\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Hello\"},\"finish_reason\":\"stop\"}],")
		TEXT("\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":1,\"total_tokens\":4}}");
	FUnrealAIChatResponse OpenAIResponse;
	FUnrealAIError OpenAIParseError;
	UnrealAIProviderAdapters::Get(OpenAIConfig.Api).ParseResponse(
		TEXT("compatible-test"),
		200,
		OpenAIResponseJson,
		OpenAIResponse,
		OpenAIParseError);
	TestFalse(TEXT("OpenAI-compatible response parsing succeeds"), OpenAIParseError.bIsError);
	TestEqual(TEXT("OpenAI-compatible response returns one choice"), OpenAIResponse.Choices.Num(), 1);
	if (OpenAIResponse.Choices.Num() == 1)
	{
		TestEqual(TEXT("OpenAI-compatible response normalizes text"), OpenAIResponse.Choices[0].Content, FString(TEXT("Hello")));
		TestEqual(TEXT("OpenAI-compatible response normalizes finish reason"), OpenAIResponse.Choices[0].FinishReason, FString(TEXT("stop")));
	}
	TestEqual(TEXT("OpenAI-compatible response normalizes total usage"), OpenAIResponse.Usage.TotalTokens, 4);

	FUnrealAIProviderConfig AnthropicConfig;
	AnthropicConfig.Api = EUnrealAIProviderApi::AnthropicMessages;
	AnthropicConfig.BaseUrl = TEXT("https://api.anthropic.com/v1/");
	AnthropicConfig.DefaultModel = TEXT("claude-test");
	FUnrealAIHttpRequestData AnthropicRequest;
	FUnrealAIError AnthropicBuildError;
	TestTrue(
		TEXT("Anthropic request construction succeeds"),
		UnrealAIProviderAdapters::Get(AnthropicConfig.Api).BuildRequest(
			AnthropicConfig,
			Request,
			TEXT("anthropic-test-key"),
			AnthropicRequest,
			AnthropicBuildError));
	TestFalse(TEXT("Anthropic request construction has no error"), AnthropicBuildError.bIsError);
	TestEqual(TEXT("Anthropic uses the native Messages endpoint"), AnthropicRequest.Url, FString(TEXT("https://api.anthropic.com/v1/messages")));
	TestEqual(TEXT("Anthropic uses x-api-key authentication"), AnthropicRequest.Headers.FindRef(TEXT("x-api-key")), FString(TEXT("anthropic-test-key")));
	TestEqual(TEXT("Anthropic sends the required API version"), AnthropicRequest.Headers.FindRef(TEXT("anthropic-version")), FString(TEXT("2023-06-01")));
	TSharedPtr<FJsonObject> AnthropicPayload;
	TestTrue(
		TEXT("Anthropic serializes a valid JSON object"),
		UnrealAIAutomationTestsPrivate::ParseJsonObject(AnthropicRequest.Body, AnthropicPayload));
	if (AnthropicPayload.IsValid())
	{
		FString SerializedModel;
		int32 SerializedMaxTokens = 0;
		TestTrue(TEXT("Anthropic includes its model field"), AnthropicPayload->TryGetStringField(TEXT("model"), SerializedModel));
		TestEqual(TEXT("Anthropic serializes the configured model"), SerializedModel, FString(TEXT("claude-test")));
		TestTrue(TEXT("Anthropic includes max_tokens"), AnthropicPayload->TryGetNumberField(TEXT("max_tokens"), SerializedMaxTokens));
		TestEqual(TEXT("Anthropic serializes max_tokens"), SerializedMaxTokens, 64);
		TestTrue(TEXT("Anthropic maps system messages to system blocks"), AnthropicPayload->HasTypedField<EJson::Array>(TEXT("system")));
		TestTrue(TEXT("Anthropic maps stop sequences"), AnthropicPayload->HasTypedField<EJson::Array>(TEXT("stop_sequences")));
	}

	const FString AnthropicResponseJson =
		TEXT("{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",")
		TEXT("\"content\":[{\"type\":\"text\",\"text\":\"Hello \"},{\"type\":\"text\",\"text\":\"world\"}],")
		TEXT("\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":7,\"output_tokens\":2}}");
	FUnrealAIChatResponse AnthropicResponse;
	FUnrealAIError AnthropicParseError;
	UnrealAIProviderAdapters::Get(AnthropicConfig.Api).ParseResponse(
		TEXT("claude-test"),
		200,
		AnthropicResponseJson,
		AnthropicResponse,
		AnthropicParseError);
	TestFalse(TEXT("Anthropic response parsing succeeds"), AnthropicParseError.bIsError);
	TestEqual(TEXT("Anthropic returns one normalized choice"), AnthropicResponse.Choices.Num(), 1);
	if (AnthropicResponse.Choices.Num() == 1)
	{
		TestEqual(TEXT("Anthropic text blocks are concatenated"), AnthropicResponse.Choices[0].Content, FString(TEXT("Hello world")));
		TestEqual(TEXT("Anthropic stop reason is normalized"), AnthropicResponse.Choices[0].FinishReason, FString(TEXT("end_turn")));
	}
	TestEqual(TEXT("Anthropic prompt usage is normalized"), AnthropicResponse.Usage.PromptTokens, 7);
	TestEqual(TEXT("Anthropic completion usage is normalized"), AnthropicResponse.Usage.CompletionTokens, 2);
	TestEqual(TEXT("Anthropic total usage is calculated"), AnthropicResponse.Usage.TotalTokens, 9);

	FUnrealAIProviderConfig GeminiConfig;
	GeminiConfig.Api = EUnrealAIProviderApi::GeminiGenerateContent;
	GeminiConfig.BaseUrl = TEXT("https://generativelanguage.googleapis.com/v1beta");
	GeminiConfig.DefaultModel = TEXT("models/gemini-test");
	FUnrealAIHttpRequestData GeminiRequest;
	FUnrealAIError GeminiBuildError;
	TestTrue(
		TEXT("Gemini request construction succeeds"),
		UnrealAIProviderAdapters::Get(GeminiConfig.Api).BuildRequest(
			GeminiConfig,
			Request,
			TEXT("gemini-test-key"),
			GeminiRequest,
			GeminiBuildError));
	TestFalse(TEXT("Gemini request construction has no error"), GeminiBuildError.bIsError);
	TestEqual(
		TEXT("Gemini uses the native generateContent endpoint"),
		GeminiRequest.Url,
		FString(TEXT("https://generativelanguage.googleapis.com/v1beta/models/gemini-test:generateContent")));
	TestEqual(TEXT("Gemini uses x-goog-api-key authentication"), GeminiRequest.Headers.FindRef(TEXT("x-goog-api-key")), FString(TEXT("gemini-test-key")));
	TSharedPtr<FJsonObject> GeminiPayload;
	TestTrue(
		TEXT("Gemini serializes a valid JSON object"),
		UnrealAIAutomationTestsPrivate::ParseJsonObject(GeminiRequest.Body, GeminiPayload));
	if (GeminiPayload.IsValid())
	{
		TestTrue(TEXT("Gemini maps system messages to systemInstruction"), GeminiPayload->HasTypedField<EJson::Object>(TEXT("systemInstruction")));
		TestTrue(TEXT("Gemini maps messages to contents"), GeminiPayload->HasTypedField<EJson::Array>(TEXT("contents")));
		const TSharedPtr<FJsonObject>* GenerationConfig = nullptr;
		TestTrue(
			TEXT("Gemini includes generationConfig"),
			GeminiPayload->TryGetObjectField(TEXT("generationConfig"), GenerationConfig));
		if (GenerationConfig && GenerationConfig->IsValid())
		{
			int32 SerializedMaxOutputTokens = 0;
			TestTrue(
				TEXT("Gemini includes maxOutputTokens"),
				(*GenerationConfig)->TryGetNumberField(TEXT("maxOutputTokens"), SerializedMaxOutputTokens));
			TestEqual(TEXT("Gemini maps max tokens to generationConfig"), SerializedMaxOutputTokens, 64);
		}
	}

	const FString GeminiResponseJson =
		TEXT("{\"responseId\":\"response-1\",\"modelVersion\":\"gemini-test\",")
		TEXT("\"candidates\":[{\"index\":0,\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Hello \"},{\"text\":\"world\"}]},\"finishReason\":\"STOP\"}],")
		TEXT("\"usageMetadata\":{\"promptTokenCount\":4,\"candidatesTokenCount\":2,\"totalTokenCount\":6}}");
	FUnrealAIChatResponse GeminiResponse;
	FUnrealAIError GeminiParseError;
	UnrealAIProviderAdapters::Get(GeminiConfig.Api).ParseResponse(
		TEXT("gemini-test"),
		200,
		GeminiResponseJson,
		GeminiResponse,
		GeminiParseError);
	TestFalse(TEXT("Gemini response parsing succeeds"), GeminiParseError.bIsError);
	TestEqual(TEXT("Gemini returns one normalized choice"), GeminiResponse.Choices.Num(), 1);
	if (GeminiResponse.Choices.Num() == 1)
	{
		TestEqual(TEXT("Gemini text parts are concatenated"), GeminiResponse.Choices[0].Content, FString(TEXT("Hello world")));
		TestEqual(TEXT("Gemini finish reason is normalized"), GeminiResponse.Choices[0].FinishReason, FString(TEXT("STOP")));
	}
	TestEqual(TEXT("Gemini prompt usage is normalized"), GeminiResponse.Usage.PromptTokens, 4);
	TestEqual(TEXT("Gemini completion usage is normalized"), GeminiResponse.Usage.CompletionTokens, 2);
	TestEqual(TEXT("Gemini total usage is normalized"), GeminiResponse.Usage.TotalTokens, 6);

	FUnrealAIChatResponse ErrorResponse;
	FUnrealAIError ProviderError;
	UnrealAIProviderAdapters::Get(GeminiConfig.Api).ParseResponse(
		TEXT("gemini-test"),
		400,
		TEXT("{\"error\":{\"code\":400,\"message\":\"Invalid request\",\"status\":\"INVALID_ARGUMENT\"}}"),
		ErrorResponse,
		ProviderError);
	TestTrue(TEXT("Provider errors are surfaced"), ProviderError.bIsError);
	TestEqual(TEXT("Provider HTTP status is preserved"), ProviderError.HttpStatus, 400);
	TestEqual(TEXT("Provider error messages are parsed"), ProviderError.Message, FString(TEXT("Invalid request")));
	TestEqual(TEXT("Provider error types are parsed"), ProviderError.Type, FString(TEXT("INVALID_ARGUMENT")));
	TestEqual(TEXT("Numeric provider error codes are normalized"), ProviderError.Code, FString(TEXT("400")));

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
