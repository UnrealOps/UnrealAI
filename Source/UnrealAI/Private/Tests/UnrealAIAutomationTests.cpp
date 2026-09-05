#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIChatCompletionAsyncAction.h"
#include "UnrealAIChatComponent.h"
#include "UnrealAIChatStreamAsyncAction.h"
#include "UnrealAIClient.h"
#include "Tests/UnrealAIChatComponentTestSupport.h"
#include "Tests/UnrealAIClientTestSupport.h"
#include "UnrealAIProviderAdapter.h"
#include "UnrealAIProviders.h"
#include "UnrealAIRetryPolicy.h"
#include "UnrealAISettings.h"
#include "UnrealAISseParser.h"
#include "UnrealAIStreamChunkQueue.h"
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
	FUnrealAIChatComponentRequestConstructionTest,
	"UnrealAI.ChatComponent.RequestConstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIChatComponentRequestConstructionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FUnrealAIChatComponentTestAccess::RunRequestConstructionTests(*this);
	return true;
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
	OpenAIRequest.AdditionalParametersJson =
		TEXT("{\"seed\":42,\"game_request_id\":\"00000000-0000-0000-0000-000000000042\"}");
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
			EUnrealAIRequestMode::OneShot,
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
		FString GameRequestId;
		TestTrue(TEXT("OpenAI-compatible request includes n"), OpenAIPayload->TryGetNumberField(TEXT("n"), NumChoices));
		TestEqual(TEXT("OpenAI-compatible request maps choice count"), NumChoices, 2);
		TestTrue(TEXT("OpenAI-compatible request merges additional parameters"), OpenAIPayload->TryGetNumberField(TEXT("seed"), Seed));
		TestEqual(TEXT("OpenAI-compatible additional parameters preserve values"), Seed, 42);
		TestTrue(
			TEXT("OpenAI-compatible wire JSON contains the backend idempotency field"),
			OpenAIPayload->TryGetStringField(TEXT("game_request_id"), GameRequestId));
		TestEqual(
			TEXT("OpenAI-compatible wire JSON preserves the backend idempotency value"),
			GameRequestId,
			FString(TEXT("00000000-0000-0000-0000-000000000042")));
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
			EUnrealAIRequestMode::OneShot,
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
			EUnrealAIRequestMode::OneShot,
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
	FUnrealAISseParserTest,
	"UnrealAI.Streaming.SseParser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAISseParserTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FString Source =
		TEXT(": heartbeat\r\nevent: content\r\ndata: {\"text\":\"hé\"}\r\n")
		TEXT("data: second line\r\nid: event-1\r\n\r\ndata: final\n\n");
	const FTCHARToUTF8 Utf8(*Source);
	FUnrealAISseParser Parser;
	TArray<FUnrealAISseEvent> Events;
	FUnrealAIError Error;
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		if (!Parser.Append(reinterpret_cast<const uint8*>(Utf8.Get()) + Index, 1, Events, Error))
		{
			break;
		}
	}
	TestFalse(TEXT("Fragmented SSE input parses without error"), Error.bIsError);
	TestTrue(TEXT("Finishing a complete SSE stream succeeds"), Parser.Finish(Events, Error));
	TestEqual(TEXT("Comments do not create events"), Events.Num(), 2);
	if (Events.Num() == 2)
	{
		TestEqual(TEXT("Named event type is preserved"), Events[0].EventType, FString(TEXT("content")));
		TestEqual(
			TEXT("Multiline SSE data is joined with a newline"),
			Events[0].Data,
			FString(TEXT("{\"text\":\"hé\"}\nsecond line")));
		TestEqual(TEXT("SSE event IDs are preserved"), Events[0].Id, FString(TEXT("event-1")));
		TestEqual(TEXT("Unnamed events default to message"), Events[1].EventType, FString(TEXT("message")));
		TestEqual(TEXT("Final event data is preserved"), Events[1].Data, FString(TEXT("final")));
	}

	TArray<uint8> Oversized;
	Oversized.SetNumZeroed(FUnrealAISseParser::MaxEventBytes + 1);
	FUnrealAISseParser OversizedParser;
	TArray<FUnrealAISseEvent> IgnoredEvents;
	FUnrealAIError OversizedError;
	TestFalse(
		TEXT("Oversized SSE events are rejected"),
		OversizedParser.Append(Oversized.GetData(), Oversized.Num(), IgnoredEvents, OversizedError));
	TestEqual(TEXT("Oversized SSE error has a stable code"), OversizedError.Code, FString(TEXT("invalid_sse")));

	const FString LargeData = FString::ChrN(600 * 1024, TEXT('x'));
	const FString MultipleLargeEvents =
		FString::Printf(TEXT("data: %s\n\ndata: %s\n\n"), *LargeData, *LargeData);
	const FTCHARToUTF8 MultipleLargeUtf8(*MultipleLargeEvents);
	FUnrealAISseParser MultipleLargeParser;
	TArray<FUnrealAISseEvent> MultipleLargeParsedEvents;
	FUnrealAIError MultipleLargeError;
	TestTrue(
		TEXT("A large network chunk containing individually bounded events is accepted"),
		MultipleLargeParser.Append(
			reinterpret_cast<const uint8*>(MultipleLargeUtf8.Get()),
			MultipleLargeUtf8.Length(),
			MultipleLargeParsedEvents,
			MultipleLargeError));
	TestEqual(TEXT("Both bounded events are emitted"), MultipleLargeParsedEvents.Num(), 2);

	const FString FragmentedData = FString::ChrN(64 * 1024, TEXT('y'));
	const FString FragmentedEvent = FString::Printf(TEXT("data: %s\r\n\r\n"), *FragmentedData);
	const FTCHARToUTF8 FragmentedUtf8(*FragmentedEvent);
	FUnrealAISseParser FragmentedParser;
	TArray<FUnrealAISseEvent> FragmentedEvents;
	FUnrealAIError FragmentedError;
	for (int32 Index = 0; Index < FragmentedUtf8.Length(); ++Index)
	{
		if (!FragmentedParser.Append(
			reinterpret_cast<const uint8*>(FragmentedUtf8.Get()) + Index,
			1,
			FragmentedEvents,
			FragmentedError))
		{
			break;
		}
	}
	TestFalse(TEXT("A long byte-fragmented SSE event parses without error"), FragmentedError.bIsError);
	TestTrue(TEXT("A long byte-fragmented SSE stream finishes"), FragmentedParser.Finish(FragmentedEvents, FragmentedError));
	TestEqual(TEXT("The fragmented stream emits one event"), FragmentedEvents.Num(), 1);
	if (FragmentedEvents.Num() == 1)
	{
		TestEqual(TEXT("The fragmented event preserves its full data field"), FragmentedEvents[0].Data, FragmentedData);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIStreamChunkQueueTest,
	"UnrealAI.Streaming.ChunkQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIStreamChunkQueueTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const uint8 FirstChunk[] = {1, 2, 3};
	const uint8 SecondChunk[] = {4, 5};
	FUnrealAIStreamChunkQueue Queue(4);
	TestTrue(TEXT("A chunk within the pending byte limit is accepted"), Queue.Enqueue(FirstChunk, 3));
	TestEqual(TEXT("The queue tracks accepted bytes"), Queue.GetQueuedByteCount(), int64(3));
	TestFalse(TEXT("A chunk that would exceed the pending byte limit is rejected"), Queue.Enqueue(SecondChunk, 2));
	TestEqual(TEXT("A rejected chunk does not change the queued byte count"), Queue.GetQueuedByteCount(), int64(3));

	TArray<TArray<uint8>> DrainedChunks;
	Queue.Drain(DrainedChunks);
	TestTrue(TEXT("Draining empties the pending queue"), Queue.IsEmpty());
	TestEqual(TEXT("Draining resets the queued byte count"), Queue.GetQueuedByteCount(), int64(0));
	TestEqual(TEXT("Draining preserves accepted chunks"), DrainedChunks.Num(), 1);
	if (DrainedChunks.Num() == 1)
	{
		TestEqual(TEXT("The accepted chunk retains its bytes"), DrainedChunks[0].Num(), 3);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIRetryPolicyTest,
	"UnrealAI.Retry.Policy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIRetryPolicyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FUnrealAIRetryPolicy ProviderPolicy;
	FUnrealAIRequestRetryOptions RequestOptions;
	FUnrealAIRetryPolicy Policy = UnrealAIRetryPolicy::Resolve(ProviderPolicy, RequestOptions);
	TestEqual(TEXT("The default policy retries twice"), Policy.MaxRetries, 2);
	TestTrue(TEXT("The default initial delay is one second"), FMath::IsNearlyEqual(Policy.InitialDelaySeconds, 1.0f));
	TestTrue(TEXT("The default maximum delay is sixty seconds"), FMath::IsNearlyEqual(Policy.MaxDelaySeconds, 60.0f));

	RequestOptions.Mode = EUnrealAIRetryMode::Disabled;
	TestEqual(
		TEXT("A request can disable configured retries"),
		UnrealAIRetryPolicy::Resolve(ProviderPolicy, RequestOptions).MaxRetries,
		0);
	RequestOptions.Mode = EUnrealAIRetryMode::OverrideMaxRetries;
	RequestOptions.MaxRetries = 100;
	TestEqual(
		TEXT("A request retry override is bounded"),
		UnrealAIRetryPolicy::Resolve(ProviderPolicy, RequestOptions).MaxRetries,
		UnrealAIRetryPolicy::MaxSupportedRetries);
	ProviderPolicy.InitialDelaySeconds = 10.0f;
	ProviderPolicy.MaxDelaySeconds = 2.0f;
	Policy = UnrealAIRetryPolicy::Resolve(ProviderPolicy, FUnrealAIRequestRetryOptions());
	TestTrue(
		TEXT("The maximum delay remains a ceiling below the initial delay"),
		FMath::IsNearlyEqual(Policy.MaxDelaySeconds, 2.0f));
	ProviderPolicy = FUnrealAIRetryPolicy();
	Policy = UnrealAIRetryPolicy::Resolve(ProviderPolicy, FUnrealAIRequestRetryOptions());

	FUnrealAIRetryFailure Failure;
	Failure.Reason = EUnrealAIRetryReason::ConnectionError;
	TestTrue(TEXT("Connection failures are retryable"), UnrealAIRetryPolicy::IsRetryable(Policy, 0, Failure));
	Failure.Reason = EUnrealAIRetryReason::Timeout;
	TestTrue(TEXT("Timeouts are retryable"), UnrealAIRetryPolicy::IsRetryable(Policy, 0, Failure));
	Failure.Reason = EUnrealAIRetryReason::EmptyStream;
	TestTrue(TEXT("Empty streams are retryable before data events"), UnrealAIRetryPolicy::IsRetryable(Policy, 0, Failure));

	Failure.Reason = EUnrealAIRetryReason::HttpError;
	for (const int32 RetryableStatus : {408, 409, 429, 500, 502, 503, 504, 529})
	{
		Failure.HttpStatus = RetryableStatus;
		TestTrue(
			*FString::Printf(TEXT("HTTP %d is retryable"), RetryableStatus),
			UnrealAIRetryPolicy::IsRetryable(Policy, 0, Failure));
	}
	for (const int32 PermanentStatus : {400, 401, 403, 404, 501})
	{
		Failure.HttpStatus = PermanentStatus;
		TestFalse(
			*FString::Printf(TEXT("HTTP %d is not retryable"), PermanentStatus),
			UnrealAIRetryPolicy::IsRetryable(Policy, 0, Failure));
	}

	Failure.HttpStatus = 429;
	Failure.Error.Code = TEXT("insufficient_quota");
	TestFalse(
		TEXT("Permanent OpenAI quota failures are not retried"),
		UnrealAIRetryPolicy::IsRetryable(Policy, 0, Failure));
	Failure.Error = FUnrealAIError();
	Failure.Error.RawJson = TEXT("{\"details\":{\"error_code\":\"enforced_spend_limit_reached\"}}");
	TestFalse(
		TEXT("Permanent Anthropic spend limits are not retried"),
		UnrealAIRetryPolicy::IsRetryable(Policy, 0, Failure));
	Failure.Error = FUnrealAIError();
	TestFalse(
		TEXT("The retry budget is enforced"),
		UnrealAIRetryPolicy::IsRetryable(Policy, Policy.MaxRetries, Failure));
	TestTrue(
		TEXT("A stream can retry before a complete data event"),
		UnrealAIRetryPolicy::CanRetryStream(false));
	TestFalse(
		TEXT("A stream cannot retry after a complete data event"),
		UnrealAIRetryPolicy::CanRetryStream(true));

	float DelaySeconds = 0.0f;
	const FDateTime ReferenceTime(2015, 10, 21, 7, 27, 50);
	TestTrue(
		TEXT("The first retry delay can be calculated"),
		UnrealAIRetryPolicy::TryComputeDelay(
			Policy, 1, FString(), ReferenceTime, 0.0f, DelaySeconds));
	TestTrue(TEXT("The first retry starts at one second"), FMath::IsNearlyEqual(DelaySeconds, 1.0f));
	TestTrue(
		TEXT("Additive jitter can be calculated"),
		UnrealAIRetryPolicy::TryComputeDelay(
			Policy, 2, FString(), ReferenceTime, 1.0f, DelaySeconds));
	TestTrue(TEXT("The second retry doubles and adds at most 25 percent jitter"), FMath::IsNearlyEqual(DelaySeconds, 2.5f));
	TestTrue(
		TEXT("A numeric Retry-After value is honored as a minimum"),
		UnrealAIRetryPolicy::TryComputeDelay(
			Policy, 1, TEXT("5"), ReferenceTime, 0.0f, DelaySeconds));
	TestTrue(TEXT("Retry-After raises the delay"), FMath::IsNearlyEqual(DelaySeconds, 5.0f));
	TestTrue(
		TEXT("An HTTP-date Retry-After value is supported"),
		UnrealAIRetryPolicy::TryComputeDelay(
			Policy,
			1,
			TEXT("Wed, 21 Oct 2015 07:28:00 GMT"),
			ReferenceTime,
			0.0f,
			DelaySeconds));
	TestTrue(TEXT("The HTTP date becomes a ten-second delay"), FMath::IsNearlyEqual(DelaySeconds, 10.0f));
	TestFalse(
		TEXT("A server delay above the configured ceiling is not retried"),
		UnrealAIRetryPolicy::TryComputeDelay(
			Policy, 1, TEXT("61"), ReferenceTime, 0.0f, DelaySeconds));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIRetryCoordinatorTest,
	"UnrealAI.Retry.Coordinator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIRetryCoordinatorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FUnrealAIClientTestAccess::RunRetryCoordinatorTests(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealAIStreamingAdaptersTest,
	"UnrealAI.Streaming.ProviderAdapters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIStreamingAdaptersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FUnrealAIChatRequest Request;
	FUnrealAIChatMessage Message;
	Message.Content = TEXT("Hello");
	Request.Messages.Add(Message);
	Request.AdditionalParametersJson = TEXT("{\"stream\":false}");

	FUnrealAIProviderConfig OpenAIConfig;
	OpenAIConfig.Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
	OpenAIConfig.BaseUrl = TEXT("https://compatible.example/v1");
	OpenAIConfig.DefaultModel = TEXT("test-model");
	FUnrealAIHttpRequestData OpenAIRequest;
	FUnrealAIError BuildError;
	TestTrue(
		TEXT("OpenAI-compatible streaming request builds"),
		UnrealAIProviderAdapters::Get(OpenAIConfig.Api).BuildRequest(
			OpenAIConfig,
			Request,
			TEXT("test-key"),
			EUnrealAIRequestMode::Stream,
			OpenAIRequest,
			BuildError));
	TestEqual(
		TEXT("Streaming requests ask for SSE"),
		OpenAIRequest.Headers.FindRef(TEXT("Accept")),
		FString(TEXT("text/event-stream")));
	TSharedPtr<FJsonObject> OpenAIPayload;
	TestTrue(TEXT("OpenAI stream payload is JSON"), UnrealAIAutomationTestsPrivate::ParseJsonObject(OpenAIRequest.Body, OpenAIPayload));
	if (OpenAIPayload.IsValid())
	{
		bool bStream = false;
		TestTrue(TEXT("OpenAI stream flag is present"), OpenAIPayload->TryGetBoolField(TEXT("stream"), bStream));
		TestTrue(TEXT("The dedicated API forces stream after additional parameters merge"), bStream);
	}

	const IUnrealAIProviderAdapter& OpenAIAdapter = UnrealAIProviderAdapters::Get(OpenAIConfig.Api);
	FUnrealAIProviderStreamState OpenAIState;
	OpenAIState.Response.Model = TEXT("test-model");
	TArray<FUnrealAIChatStreamEvent> OpenAIEvents;
	FUnrealAIError ParseError;
	const TArray<FUnrealAISseEvent> OpenAIFrames = {
		{TEXT("message"), TEXT("{\"id\":\"chatcmpl-1\",\"model\":\"test-model\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"Hello \"},\"finish_reason\":null}]}"), FString()},
		{TEXT("message"), TEXT("{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"world\"},\"finish_reason\":\"stop\"}]}"), FString()},
		{TEXT("message"), TEXT("{\"choices\":[],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":2,\"total_tokens\":4}}"), FString()},
		{TEXT("message"), TEXT("[DONE]"), FString()}
	};
	for (const FUnrealAISseEvent& Frame : OpenAIFrames)
	{
		TestTrue(TEXT("OpenAI stream frame parses"), OpenAIAdapter.ParseStreamEvent(Frame, OpenAIState, OpenAIEvents, ParseError));
	}
	TestTrue(TEXT("OpenAI stream reaches a terminal state"), OpenAIAdapter.CanCompleteStream(OpenAIState));
	TestEqual(TEXT("OpenAI stream creates one choice"), OpenAIState.Response.Choices.Num(), 1);
	if (OpenAIState.Response.Choices.Num() == 1)
	{
		TestEqual(TEXT("OpenAI deltas accumulate"), OpenAIState.Response.Choices[0].Content, FString(TEXT("Hello world")));
		TestEqual(TEXT("OpenAI finish reason accumulates"), OpenAIState.Response.Choices[0].FinishReason, FString(TEXT("stop")));
	}
	TestEqual(TEXT("OpenAI stream usage accumulates"), OpenAIState.Response.Usage.TotalTokens, 4);

	FUnrealAIProviderStreamState MultiChoiceOpenAIState;
	MultiChoiceOpenAIState.ExpectedChoiceCount = 2;
	TArray<FUnrealAIChatStreamEvent> MultiChoiceEvents;
	TestTrue(
		TEXT("The first choice finish parses"),
		OpenAIAdapter.ParseStreamEvent(
			{TEXT("message"), TEXT("{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"), FString()},
			MultiChoiceOpenAIState,
			MultiChoiceEvents,
			ParseError));
	TestFalse(
		TEXT("A multi-choice compatible stream does not complete after only one choice finishes"),
		OpenAIAdapter.CanCompleteStream(MultiChoiceOpenAIState));
	TestTrue(
		TEXT("The second choice finish parses"),
		OpenAIAdapter.ParseStreamEvent(
			{TEXT("message"), TEXT("{\"choices\":[{\"index\":1,\"delta\":{},\"finish_reason\":\"stop\"}]}"), FString()},
			MultiChoiceOpenAIState,
			MultiChoiceEvents,
			ParseError));
	TestTrue(
		TEXT("A compatible stream may complete after every expected choice finishes"),
		OpenAIAdapter.CanCompleteStream(MultiChoiceOpenAIState));

	FUnrealAIProviderConfig AnthropicConfig;
	AnthropicConfig.Api = EUnrealAIProviderApi::AnthropicMessages;
	AnthropicConfig.BaseUrl = TEXT("https://api.anthropic.com/v1");
	AnthropicConfig.DefaultModel = TEXT("claude-test");
	FUnrealAIHttpRequestData AnthropicRequest;
	Request.AdditionalParametersJson.Reset();
	TestTrue(
		TEXT("Anthropic streaming request builds"),
		UnrealAIProviderAdapters::Get(AnthropicConfig.Api).BuildRequest(
			AnthropicConfig,
			Request,
			TEXT("test-key"),
			EUnrealAIRequestMode::Stream,
			AnthropicRequest,
			BuildError));
	TSharedPtr<FJsonObject> AnthropicPayload;
	TestTrue(TEXT("Anthropic stream payload is JSON"), UnrealAIAutomationTestsPrivate::ParseJsonObject(AnthropicRequest.Body, AnthropicPayload));
	if (AnthropicPayload.IsValid())
	{
		bool bStream = false;
		TestTrue(TEXT("Anthropic stream flag is present"), AnthropicPayload->TryGetBoolField(TEXT("stream"), bStream));
		TestTrue(TEXT("Anthropic streaming is enabled"), bStream);
	}

	const IUnrealAIProviderAdapter& AnthropicAdapter = UnrealAIProviderAdapters::Get(AnthropicConfig.Api);
	FUnrealAIProviderStreamState AnthropicState;
	TArray<FUnrealAIChatStreamEvent> AnthropicEvents;
	const TArray<FUnrealAISseEvent> AnthropicFrames = {
		{TEXT("message_start"), TEXT("{\"type\":\"message_start\",\"message\":{\"id\":\"msg-1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-test\",\"usage\":{\"input_tokens\":3,\"output_tokens\":0}}}"), FString()},
		{TEXT("ping"), TEXT("{\"type\":\"ping\"}"), FString()},
		{TEXT("content_block_delta"), TEXT("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}"), FString()},
		{TEXT("content_block_start"), TEXT("{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}"), FString()},
		{TEXT("content_block_delta"), TEXT("{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hidden\"}}"), FString()},
		{TEXT("content_block_stop"), TEXT("{\"type\":\"content_block_stop\",\"index\":1}"), FString()},
		{TEXT("message_delta"), TEXT("{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":1}}"), FString()},
		{TEXT("message_stop"), TEXT("{\"type\":\"message_stop\"}"), FString()}
	};
	for (const FUnrealAISseEvent& Frame : AnthropicFrames)
	{
		TestTrue(TEXT("Anthropic stream frame parses"), AnthropicAdapter.ParseStreamEvent(Frame, AnthropicState, AnthropicEvents, ParseError));
	}
	TestTrue(TEXT("Anthropic requires and sees message_stop"), AnthropicAdapter.CanCompleteStream(AnthropicState));
	TestEqual(TEXT("Anthropic text deltas accumulate"), AnthropicState.Response.Choices[0].Content, FString(TEXT("Hello")));
	TestEqual(TEXT("Anthropic cumulative usage is normalized"), AnthropicState.Response.Usage.TotalTokens, 4);
	TestTrue(
		TEXT("Non-text Anthropic deltas remain visible as provider events"),
		AnthropicEvents.ContainsByPredicate([](const FUnrealAIChatStreamEvent& Event)
		{
			return Event.Type == EUnrealAIChatStreamEventType::ProviderEvent;
		}));
	TestTrue(
		TEXT("Anthropic content block closure remains visible as a provider event"),
		AnthropicEvents.ContainsByPredicate([](const FUnrealAIChatStreamEvent& Event)
		{
			return Event.Type == EUnrealAIChatStreamEventType::ProviderEvent
				&& Event.ProviderEventType == TEXT("content_block_stop")
				&& Event.RawJson.Contains(TEXT("\"index\":1"));
		}));

	FUnrealAIProviderConfig GeminiConfig;
	GeminiConfig.Api = EUnrealAIProviderApi::GeminiGenerateContent;
	GeminiConfig.BaseUrl = TEXT("https://generativelanguage.googleapis.com/v1beta");
	GeminiConfig.DefaultModel = TEXT("models/gemini-test");
	FUnrealAIHttpRequestData GeminiRequest;
	TestTrue(
		TEXT("Gemini streaming request builds"),
		UnrealAIProviderAdapters::Get(GeminiConfig.Api).BuildRequest(
			GeminiConfig,
			Request,
			TEXT("test-key"),
			EUnrealAIRequestMode::Stream,
			GeminiRequest,
			BuildError));
	TestEqual(
		TEXT("Gemini uses streamGenerateContent SSE endpoint"),
		GeminiRequest.Url,
		FString(TEXT("https://generativelanguage.googleapis.com/v1beta/models/gemini-test:streamGenerateContent?alt=sse")));

	const IUnrealAIProviderAdapter& GeminiAdapter = UnrealAIProviderAdapters::Get(GeminiConfig.Api);
	FUnrealAIProviderStreamState GeminiState;
	TArray<FUnrealAIChatStreamEvent> GeminiEvents;
	const TArray<FUnrealAISseEvent> GeminiFrames = {
		{TEXT("message"), TEXT("{\"responseId\":\"response-1\",\"modelVersion\":\"gemini-test\",\"candidates\":[{\"index\":0,\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"Hello \"}]}}]}"), FString()},
		{TEXT("message"), TEXT("{\"candidates\":[{\"index\":0,\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"world\"}]},\"finishReason\":\"STOP\",\"safetyRatings\":[{\"category\":\"HARM_CATEGORY_HARASSMENT\",\"probability\":\"NEGLIGIBLE\"}],\"groundingMetadata\":{\"webSearchQueries\":[\"example\"]}}],\"usageMetadata\":{\"promptTokenCount\":2,\"candidatesTokenCount\":2,\"totalTokenCount\":4}}"), FString()}
	};
	for (const FUnrealAISseEvent& Frame : GeminiFrames)
	{
		TestTrue(TEXT("Gemini stream frame parses"), GeminiAdapter.ParseStreamEvent(Frame, GeminiState, GeminiEvents, ParseError));
	}
	TestTrue(TEXT("Gemini completes at successful HTTP EOF"), GeminiAdapter.CanCompleteStream(GeminiState));
	TestEqual(TEXT("Gemini text chunks accumulate"), GeminiState.Response.Choices[0].Content, FString(TEXT("Hello world")));
	TestEqual(TEXT("Gemini usage is normalized"), GeminiState.Response.Usage.TotalTokens, 4);
	TestTrue(
		TEXT("Gemini metadata accompanying normalized text remains visible as a provider event"),
		GeminiEvents.ContainsByPredicate([](const FUnrealAIChatStreamEvent& Event)
		{
			return Event.Type == EUnrealAIChatStreamEventType::ProviderEvent
				&& Event.RawJson.Contains(TEXT("groundingMetadata"))
				&& Event.RawJson.Contains(TEXT("safetyRatings"));
		}));
	FUnrealAIProviderStreamState EmptyGeminiState;
	TestFalse(TEXT("Gemini requires at least one valid data event"), GeminiAdapter.CanCompleteStream(EmptyGeminiState));

	FUnrealAIProviderStreamState TruncatedAnthropicState;
	TestFalse(TEXT("Anthropic streams without message_stop are incomplete"), AnthropicAdapter.CanCompleteStream(TruncatedAnthropicState));
	FUnrealAIError ProviderStreamError;
	TArray<FUnrealAIChatStreamEvent> IgnoredEvents;
	TestFalse(
		TEXT("Provider errors inside an Anthropic stream fail parsing"),
		AnthropicAdapter.ParseStreamEvent(
			{TEXT("error"), TEXT("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"), FString()},
			TruncatedAnthropicState,
			IgnoredEvents,
			ProviderStreamError));
	TestTrue(TEXT("In-stream provider errors are surfaced"), ProviderStreamError.bIsError);

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

	const UFunction* StreamRequest = UUnrealAIChatStreamAsyncAction::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatStreamAsyncAction, StreamChatCompletion));
	TestNotNull(TEXT("Stream Chat Completion is reflected"), StreamRequest);
	if (StreamRequest)
	{
		TestTrue(TEXT("Stream Chat Completion is Blueprint callable"), StreamRequest->HasAnyFunctionFlags(FUNC_BlueprintCallable));
#if WITH_METADATA
		TestEqual(
			TEXT("Stream Chat Completion has the documented display name"),
			StreamRequest->GetMetaData(TEXT("DisplayName")),
			FString(TEXT("Stream Chat Completion (UnrealAI)")));
#endif
	}

	const UFunction* CancelStreamRequest = UUnrealAIChatStreamAsyncAction::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatStreamAsyncAction, Cancel));
	TestNotNull(TEXT("The streaming async action exposes cancellation"), CancelStreamRequest);
	if (CancelStreamRequest)
	{
		TestTrue(TEXT("Async stream cancellation is Blueprint callable"), CancelStreamRequest->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	const UFunction* CancelCompletionRequest = UUnrealAIChatCompletionAsyncAction::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatCompletionAsyncAction, Cancel));
	TestNotNull(TEXT("The completion async action exposes cancellation"), CancelCompletionRequest);
	if (CancelCompletionRequest)
	{
		TestTrue(TEXT("Async completion cancellation is Blueprint callable"), CancelCompletionRequest->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	for (const FName DelegateName : {
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatCompletionAsyncAction, Completed),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatCompletionAsyncAction, Failed),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatCompletionAsyncAction, Cancelled),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatCompletionAsyncAction, Retrying)})
	{
		const FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(
			UUnrealAIChatCompletionAsyncAction::StaticClass(), DelegateName);
		TestNotNull(*FString::Printf(TEXT("Completion async delegate %s is reflected"), *DelegateName.ToString()), Delegate);
		if (Delegate)
		{
			TestTrue(
				*FString::Printf(TEXT("Completion async delegate %s is Blueprint assignable"), *DelegateName.ToString()),
				Delegate->HasAnyPropertyFlags(CPF_BlueprintAssignable));
		}
	}

	for (const FName DelegateName : {
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatStreamAsyncAction, Event),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatStreamAsyncAction, Completed),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatStreamAsyncAction, Failed),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatStreamAsyncAction, Cancelled),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatStreamAsyncAction, Retrying)})
	{
		const FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(
			UUnrealAIChatStreamAsyncAction::StaticClass(), DelegateName);
		TestNotNull(*FString::Printf(TEXT("Streaming async delegate %s is reflected"), *DelegateName.ToString()), Delegate);
		if (Delegate)
		{
			TestTrue(
				*FString::Printf(TEXT("Streaming async delegate %s is Blueprint assignable"), *DelegateName.ToString()),
				Delegate->HasAnyPropertyFlags(CPF_BlueprintAssignable));
		}
	}

	const UFunction* SendPrompt = UUnrealAIChatComponent::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatComponent, SendPrompt));
	TestNotNull(TEXT("Send Prompt is reflected"), SendPrompt);
	if (SendPrompt)
	{
		TestTrue(TEXT("Send Prompt is Blueprint callable"), SendPrompt->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	const UFunction* SendPromptStream = UUnrealAIChatComponent::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatComponent, SendPromptStream));
	TestNotNull(TEXT("Send Prompt Stream is reflected"), SendPromptStream);
	if (SendPromptStream)
	{
		TestTrue(TEXT("Send Prompt Stream is Blueprint callable"), SendPromptStream->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	const UFunction* CancelActiveStream = UUnrealAIChatComponent::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatComponent, CancelActiveStream));
	TestNotNull(TEXT("Cancel Active Stream is reflected"), CancelActiveStream);
	if (CancelActiveStream)
	{
		TestTrue(TEXT("Cancel Active Stream is Blueprint callable"), CancelActiveStream->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	const UFunction* CancelActiveCompletions = UUnrealAIChatComponent::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UUnrealAIChatComponent, CancelActiveCompletions));
	TestNotNull(TEXT("Cancel Active Completions is reflected"), CancelActiveCompletions);
	if (CancelActiveCompletions)
	{
		TestTrue(TEXT("Cancel Active Completions is Blueprint callable"), CancelActiveCompletions->HasAnyFunctionFlags(FUNC_BlueprintCallable));
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

	for (const FName DelegateName : {
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatCancelled),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatRetrying),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatStreamRetrying)})
	{
		const FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(
			UUnrealAIChatComponent::StaticClass(), DelegateName);
		TestNotNull(*FString::Printf(TEXT("Component delegate %s is reflected"), *DelegateName.ToString()), Delegate);
		if (Delegate)
		{
			TestTrue(
				*FString::Printf(TEXT("Component delegate %s is Blueprint assignable"), *DelegateName.ToString()),
				Delegate->HasAnyPropertyFlags(CPF_BlueprintAssignable));
		}
	}

	const FMulticastDelegateProperty* StreamEvent = FindFProperty<FMulticastDelegateProperty>(
		UUnrealAIChatComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatStreamEvent));
	TestNotNull(TEXT("On Chat Stream Event is reflected"), StreamEvent);
	if (StreamEvent)
	{
		TestTrue(TEXT("On Chat Stream Event is Blueprint assignable"), StreamEvent->HasAnyPropertyFlags(CPF_BlueprintAssignable));
	}

	const FMulticastDelegateProperty* StreamCancelled = FindFProperty<FMulticastDelegateProperty>(
		UUnrealAIChatComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatStreamCancelled));
	TestNotNull(TEXT("On Chat Stream Cancelled is reflected"), StreamCancelled);
	if (StreamCancelled)
	{
		TestTrue(TEXT("On Chat Stream Cancelled is Blueprint assignable"), StreamCancelled->HasAnyPropertyFlags(CPF_BlueprintAssignable));
	}

	for (const FName DelegateName : {
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatStreamCompleted),
		GET_MEMBER_NAME_CHECKED(UUnrealAIChatComponent, OnChatStreamFailed)})
	{
		const FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(
			UUnrealAIChatComponent::StaticClass(), DelegateName);
		TestNotNull(*FString::Printf(TEXT("Component stream delegate %s is reflected"), *DelegateName.ToString()), Delegate);
		if (Delegate)
		{
			TestTrue(
				*FString::Printf(TEXT("Component stream delegate %s is Blueprint assignable"), *DelegateName.ToString()),
				Delegate->HasAnyPropertyFlags(CPF_BlueprintAssignable));
		}
	}

	TestNotNull(
		TEXT("Retry events expose their logical request handle to Blueprints"),
		FindFProperty<FStructProperty>(
			FUnrealAIRetryEvent::StaticStruct(),
			GET_MEMBER_NAME_CHECKED(FUnrealAIRetryEvent, RequestHandle)));

	return true;
}

#endif
