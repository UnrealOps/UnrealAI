#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "UnrealAIClient.h"
#include "UnrealAIResponseAdapter.h"
#include "UnrealAIResponseAsyncAction.h"
#include "UnrealAIResponseJson.h"
#include "UnrealAIResponseLibrary.h"
#include "UObject/UnrealType.h"

namespace UnrealAIResponseTests
{
	using namespace UnrealAIResponseJson;

	FUnrealAIProviderConfig Config(int32 Protocol)
	{
		FUnrealAIProviderConfig Result;
		Result.Name = TEXT("Fixture");
		Result.BaseUrl = TEXT("http://127.0.0.1:1/v1");
		Result.DefaultModel = TEXT("fixture-model");
		Result.bRequiresApiKey = false;
		Result.Api = Protocol == 2 ? EUnrealAIProviderApi::AnthropicMessages :
			(Protocol == 3 ? EUnrealAIProviderApi::GeminiGenerateContent : EUnrealAIProviderApi::OpenAICompatibleChatCompletions);
		Result.ResponseApi = Protocol == 0 ? EUnrealAIResponseApi::OpenAIResponses : EUnrealAIResponseApi::ChatProtocol;
		return Result;
	}

	FUnrealAIResponseRequest Request()
	{
		FUnrealAIResponseRequest Result = UUnrealAIResponseLibrary::MakeResponseRequest(TEXT("What is the weather?"));
		Result.Instructions = TEXT("Be concise.");
		Result.Tools.Add(UUnrealAIResponseLibrary::MakeToolDefinition(TEXT("weather"), TEXT("Read-only weather."),
			TEXT("{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"],\"additionalProperties\":false}")));
		return Result;
	}

	const TCHAR* ToolResponses[] = {
		TEXT(R"({"id":"resp_1","status":"completed","output":[{"id":"rs_1","type":"reasoning","encrypted_content":"opaque-fixture"},{"id":"fc_1","type":"function_call","call_id":"call_1","name":"weather","arguments":"{\"city\":\"Paris\"}","status":"completed"}],"usage":{"input_tokens":3,"output_tokens":4,"total_tokens":7}})"),
		TEXT(R"({"id":"chat_1","choices":[{"index":0,"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"reasoning_content":"opaque-fixture","tool_calls":[{"id":"call_1","type":"function","function":{"name":"weather","arguments":"{\"city\":\"Paris\"}"}}]}}],"usage":{"prompt_tokens":3,"completion_tokens":4,"total_tokens":7}})"),
		TEXT(R"({"id":"msg_1","type":"message","role":"assistant","content":[{"type":"thinking","thinking":"opaque-fixture","signature":"signed-fixture"},{"type":"tool_use","id":"call_1","name":"weather","input":{"city":"Paris"}}],"stop_reason":"tool_use","usage":{"input_tokens":3,"output_tokens":4}})"),
		TEXT(R"({"responseId":"gem_1","candidates":[{"index":0,"content":{"role":"model","parts":[{"text":"opaque-fixture","thought":true,"thoughtSignature":"signed-thought"},{"functionCall":{"id":"call_1","name":"weather","args":{"city":"Paris"}},"thoughtSignature":"signed-fixture"}]},"finishReason":"STOP","groundingMetadata":{"fixture":true}}],"usageMetadata":{"promptTokenCount":3,"candidatesTokenCount":4,"totalTokenCount":7}})")
	};

	FUnrealAIResponseState ParseTool(FAutomationTestBase& Test, int32 Protocol, bool bStored = false)
	{
		FUnrealAIResponseState State;
		FUnrealAIHttpRequestData Http;
		FUnrealAIError Error;
		FUnrealAIResponseRequest Input = Request();
		Input.bStore = bStored;
		Test.TestTrue(TEXT("Request builds"), UnrealAIResponseAdapters::BuildRequest(Config(Protocol), Input, FString(),
			EUnrealAIRequestMode::OneShot, Http, State.Context, Error));
		UnrealAIResponseAdapters::ParseResponse(200, ToolResponses[Protocol], State);
		return State;
	}

	bool Frame(FAutomationTestBase& Test, FUnrealAIResponseState& State, TArray<FUnrealAIResponseEvent>& Events, const TCHAR* Json)
	{
		FUnrealAISseEvent Event;
		Event.Data = Json;
		FUnrealAIError Error;
		const bool bParsed = UnrealAIResponseAdapters::ParseStreamEvent(Event, State, Events, Error);
		return Test.TestTrue(TEXT("Stream frame accepted: ") + String(Parse(Json), TEXT("type")) + TEXT(" / ") + Error.Code, bParsed);
	}

	FUnrealAIResponseState StreamState(FAutomationTestBase& Test, int32 Protocol)
	{
		FUnrealAIResponseState State;
		FUnrealAIHttpRequestData Http;
		FUnrealAIError Error;
		Test.TestTrue(TEXT("Stream request builds"), UnrealAIResponseAdapters::BuildRequest(Config(Protocol), Request(), FString(),
			EUnrealAIRequestMode::Stream, Http, State.Context, Error));
		return State;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIResponseRequestTest, "UnrealAI.Responses.Requests",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIResponseRequestTest::RunTest(const FString& Parameters)
{
	using namespace UnrealAIResponseTests;
	for (int32 Protocol = 0; Protocol < 4; ++Protocol)
	{
		FUnrealAIResponseRequest Input = Request();
		Input.OutputFormat = EUnrealAIResponseFormat::JsonSchema;
		Input.OutputSchemaJson = TEXT("{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":\"string\"}},\"required\":[\"answer\"],\"additionalProperties\":false}");
		Input.ToolChoice = EUnrealAIToolChoice::Named;
		Input.NamedTool = TEXT("weather");
		FUnrealAIHttpRequestData Http;
		FUnrealAIResponseContext Context;
		FUnrealAIError Error;
		TestTrue(TEXT("Typed tools/schema request builds for each protocol"),
			UnrealAIResponseAdapters::BuildRequest(Config(Protocol), Input, TEXT("fixture-not-a-credential"),
				EUnrealAIRequestMode::Stream, Http, Context, Error));
		const FObject Payload = Parse(Http.Body);
		TestNotNull(TEXT("Serialized JSON object"), Payload.Get());
		TestTrue(TEXT("Tools serialized"), Payload && Payload->HasField(TEXT("tools")));
		const TCHAR* Field = Protocol == 0 ? TEXT("text") : (Protocol == 1 ? TEXT("response_format") :
			(Protocol == 2 ? TEXT("output_config") : TEXT("generationConfig")));
		TestTrue(TEXT("Structured output serialized"), Payload && Payload->HasField(Field));
		TestTrue(TEXT("Protocol endpoint selected"), Http.Url.Contains(Protocol == 0 ? TEXT("/responses") :
			(Protocol == 1 ? TEXT("/chat/completions") : (Protocol == 2 ? TEXT("/messages") : TEXT(":streamGenerateContent?alt=sse")))));
		TestEqual(TEXT("Streaming accept header"), Http.Headers.FindRef(TEXT("Accept")), FString(TEXT("text/event-stream")));
		if (Protocol == 0)
		{
			TestFalse(TEXT("Local mode disables storage"), Bool(Payload, TEXT("store")));
			TestEqual(TEXT("Encrypted reasoning requested"), Array(Payload, TEXT("include")).Num(), 1);
		}
		Input.AdditionalParametersJson = TEXT("{\"stream\":false}");
		TestFalse(TEXT("Extensions cannot override transport"), UnrealAIResponseAdapters::BuildRequest(Config(Protocol), Input,
			FString(), EUnrealAIRequestMode::OneShot, Http, Context, Error));
		Input.AdditionalParametersJson = TEXT("{");
		TestFalse(TEXT("Malformed extensions rejected"), UnrealAIResponseAdapters::BuildRequest(Config(Protocol), Input,
			FString(), EUnrealAIRequestMode::OneShot, Http, Context, Error));
		Input.AdditionalParametersJson.Reset();
		Input.NamedTool = TEXT("missing");
		TestFalse(TEXT("Unknown named tool rejected"), UnrealAIResponseAdapters::BuildRequest(Config(Protocol), Input,
			FString(), EUnrealAIRequestMode::OneShot, Http, Context, Error));
	}
	FUnrealAIProviderConfig OpenAI = Config(1);
	OpenAI.Name = TEXT("OpenAI");
	OpenAI.BaseUrl = TEXT("https://api.openai.com/v1/");
	OpenAI.ResponseApi = EUnrealAIResponseApi::ProviderDefault;
	TestTrue(TEXT("Official OpenAI defaults to Responses"), UnrealAIResponseAdapters::Capabilities(OpenAI).bUsesOpenAIResponses);
	OpenAI.BaseUrl = TEXT("https://example.invalid/v1");
	TestFalse(TEXT("Custom OpenAI-compatible defaults to chat"), UnrealAIResponseAdapters::Capabilities(OpenAI).bUsesOpenAIResponses);
	OpenAI.ResponseApi = EUnrealAIResponseApi::OpenAIResponses;
	TestTrue(TEXT("Custom native Responses opt-in"), UnrealAIResponseAdapters::Capabilities(OpenAI).bUsesOpenAIResponses);
	FUnrealAIResponseRequest Input = Request();
	FUnrealAIHttpRequestData Http;
	FUnrealAIResponseContext Context;
	FUnrealAIError Error;
	Input.Tools[0].bStrict = true;
	TestFalse(TEXT("Unsupported strict Gemini tools rejected"), UnrealAIResponseAdapters::BuildRequest(Config(3), Input,
		FString(), EUnrealAIRequestMode::OneShot, Http, Context, Error));
	Input.Tools[0].bStrict = false;
	Input.OutputFormat = EUnrealAIResponseFormat::JsonObject;
	TestFalse(TEXT("Unsupported Anthropic JSON object rejected"), UnrealAIResponseAdapters::BuildRequest(Config(2), Input,
		FString(), EUnrealAIRequestMode::OneShot, Http, Context, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIResponseContinuationTest, "UnrealAI.Responses.Continuation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIResponseContinuationTest::RunTest(const FString& Parameters)
{
	using namespace UnrealAIResponseTests;
	for (int32 Protocol = 0; Protocol < 4; ++Protocol)
	{
		FUnrealAIResponseState State = ParseTool(*this, Protocol);
		TestEqual(TEXT("Tool generation completed"), State.Result.Status, EUnrealAIResponseStatus::Completed);
		TestFalse(TEXT("No tool parse error"), State.Result.Error.bIsError);
		TestEqual(TEXT("Token counts normalized"), State.Result.Response.Usage.TotalTokens, 7);
		TestEqual(TEXT("No reasoning in visible text"), UUnrealAIResponseLibrary::GetResponseText(State.Result.Response), FString());
		const TArray<FUnrealAIResponseItem> Calls = UUnrealAIResponseLibrary::GetResponseToolCalls(State.Result);
		if (!TestEqual(TEXT("Exactly one executable tool"), Calls.Num(), 1))
		{
			continue;
		}
		TestEqual(TEXT("Correct call ID"), Calls[0].CallId, FString(TEXT("call_1")));
		TestEqual(TEXT("Arguments parsed"), String(Parse(Calls[0].ArgumentsJson), TEXT("city")), FString(TEXT("Paris")));
		FUnrealAIResponseRequest Next;
		FUnrealAIError Error;
		FUnrealAIResponseItem Output = UUnrealAIResponseLibrary::MakeToolResult(Calls[0], TEXT("{\"temperature\":20}"), false);
		TestTrue(TEXT("Continuation built"), UUnrealAIResponseLibrary::BuildContinuationRequest(Request(), State.Result, {Output}, Next, Error));
		TestTrue(TEXT("Opaque provider data retained"), Next.History.ItemsJson.Contains(TEXT("opaque-fixture")));
		if (Protocol >= 2)
		{
			TestTrue(TEXT("Signatures retained"), Next.History.ItemsJson.Contains(TEXT("signed-fixture")));
		}
		FUnrealAIHttpRequestData Http;
		FUnrealAIResponseContext Context;
		TestTrue(TEXT("Continuation serializes"), UnrealAIResponseAdapters::BuildRequest(Config(Protocol), Next,
			FString(), EUnrealAIRequestMode::OneShot, Http, Context, Error));
		TestTrue(TEXT("Tool output serialized"), Http.Body.Contains(TEXT("temperature")));
		TestTrue(TEXT("Original call replayed"), Http.Body.Contains(TEXT("weather")));
		FUnrealAIProviderConfig Other = Config(Protocol);
		Other.DefaultModel = TEXT("different-model");
		TestFalse(TEXT("Wrong-model history rejected"), UnrealAIResponseAdapters::BuildRequest(Other, Next,
			FString(), EUnrealAIRequestMode::OneShot, Http, Context, Error));
		Other = Config(Protocol);
		Other.BaseUrl = TEXT("http://127.0.0.1:2/v1");
		TestFalse(TEXT("Wrong-endpoint history rejected"), UnrealAIResponseAdapters::BuildRequest(Other, Next,
			FString(), EUnrealAIRequestMode::OneShot, Http, Context, Error));
		TestFalse(TEXT("Missing pending result rejected"), UUnrealAIResponseLibrary::BuildContinuationRequest(Request(),
			State.Result, {UUnrealAIResponseLibrary::MakeResponseMessage(TEXT("continue"))}, Next, Error));
		TestFalse(TEXT("Duplicate results rejected"), UUnrealAIResponseLibrary::BuildContinuationRequest(Request(),
			State.Result, {Output, Output}, Next, Error));
		State.Result.Status = EUnrealAIResponseStatus::Incomplete;
		TestTrue(TEXT("Incomplete result exposes no executable calls"), UUnrealAIResponseLibrary::GetResponseToolCalls(State.Result).IsEmpty());
	}
	FUnrealAIResponseState Stored = ParseTool(*this, 0, true);
	FUnrealAIResponseRequest Input = Request();
	Input.bStore = true;
	FUnrealAIResponseRequest Next;
	FUnrealAIError Error;
	const FUnrealAIResponseItem Call = UUnrealAIResponseLibrary::GetResponseToolCalls(Stored.Result)[0];
	TestTrue(TEXT("Stored continuation built"), UUnrealAIResponseLibrary::BuildContinuationRequest(Input, Stored.Result,
		{UUnrealAIResponseLibrary::MakeToolResult(Call, TEXT("20"), false)}, Next, Error));
	TestEqual(TEXT("Previous response reference"), Next.PreviousResponseId, FString(TEXT("resp_1")));
	TestTrue(TEXT("No duplicate local history in stored mode"), Next.History.ItemsJson.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIResponseStreamTest, "UnrealAI.Responses.Streaming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIResponseStreamTest::RunTest(const FString& Parameters)
{
	using namespace UnrealAIResponseTests;
	FUnrealAIResponseState Native = StreamState(*this, 0);
	TArray<FUnrealAIResponseEvent> Events;
	Frame(*this, Native, Events, TEXT(R"({"type":"response.created","response":{"id":"r","status":"in_progress","output":[]}})"));
	Frame(*this, Native, Events, TEXT(R"({"type":"response.output_item.added","output_index":0,"item":{"id":"m","type":"message","role":"assistant","content":[]}})"));
	Frame(*this, Native, Events, TEXT(R"({"type":"response.output_text.delta","output_index":0,"content_index":0,"delta":"Hello "})"));
	Frame(*this, Native, Events, TEXT(R"({"type":"response.output_item.added","output_index":1,"item":{"id":"f1","type":"function_call","call_id":"c1","name":"weather","arguments":""}})"));
	Frame(*this, Native, Events, TEXT(R"({"type":"response.output_item.added","output_index":2,"item":{"id":"f2","type":"function_call","call_id":"c2","name":"weather","arguments":""}})"));
	Frame(*this, Native, Events, TEXT(R"({"type":"response.function_call_arguments.delta","output_index":1,"delta":"{\"city\":"})"));
	Frame(*this, Native, Events, TEXT(R"({"type":"response.function_call_arguments.delta","output_index":2,"delta":"{}"})"));
	Frame(*this, Native, Events, TEXT(R"({"type":"response.function_call_arguments.delta","output_index":1,"delta":"\"Paris\"}"})"));
	Frame(*this, Native, Events, TEXT(R"({"type":"response.completed","response":{"id":"r","status":"completed","output":[{"id":"m","type":"message","role":"assistant","content":[{"type":"output_text","text":"Hello world"}]},{"id":"f1","type":"function_call","call_id":"c1","name":"weather","arguments":"{\"city\":\"Paris\"}"},{"id":"f2","type":"function_call","call_id":"c2","name":"weather","arguments":"{}"}],"usage":{"input_tokens":1,"output_tokens":2}}})"));
	FString StreamedText;
	FString Arguments;
	int32 CompletedItems = 0;
	for (const FUnrealAIResponseEvent& Event : Events)
	{
		if (Event.Type == EUnrealAIResponseEventType::TextDelta)
		{
			StreamedText += Event.Delta;
		}
		if (Event.Type == EUnrealAIResponseEventType::ToolArgumentsDelta && Event.ItemIndex == 1)
		{
			Arguments += Event.Delta;
		}
		CompletedItems += Event.Type == EUnrealAIResponseEventType::ItemCompleted ? 1 : 0;
	}
	TestEqual(TEXT("Final snapshot emits only missing text suffix"), StreamedText, FString(TEXT("Hello world")));
	TestEqual(TEXT("Arguments are separate and assembled once"), Arguments, FString(TEXT("{\"city\":\"Paris\"}")));
	TestEqual(TEXT("Items finish once"), CompletedItems, 3);
	TestEqual(TEXT("Parallel tool calls retained"), UUnrealAIResponseLibrary::GetResponseToolCalls(Native.Result).Num(), 2);
	TestTrue(TEXT("Native terminal recognized"), UnrealAIResponseAdapters::CanCompleteStream(Native));
	FUnrealAIResponseState Anthropic = StreamState(*this, 2);
	Events.Reset();
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"message_start","message":{"id":"a","content":[],"usage":{"input_tokens":3}}})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"opaque"}})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"signed"}})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"content_block_stop","index":0})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"c","name":"weather","input":{}}})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{ \"city\": \"Paris\" }"}})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"content_block_stop","index":1})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":4}})"));
	Frame(*this, Anthropic, Events, TEXT(R"({"type":"message_stop"})"));
	TestTrue(TEXT("Anthropic signature preserved in history"), Anthropic.Result.Response.Continuation.ItemsJson.Contains(TEXT("signed")));
	TestEqual(TEXT("Anthropic tool complete"), UUnrealAIResponseLibrary::GetResponseToolCalls(Anthropic.Result).Num(), 1);
	FUnrealAIResponseState Chat = StreamState(*this, 1);
	Events.Reset();
	Frame(*this, Chat, Events, TEXT(R"({"choices":[{"index":0,"delta":{"role":"assistant","tool_calls":[{"index":0,"id":"c","function":{"name":"weather","arguments":"{\"city\":"}}]}}]})"));
	Frame(*this, Chat, Events, TEXT(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"Paris\"}"}}]},"finish_reason":"tool_calls"}]})"));
	Frame(*this, Chat, Events, TEXT("[DONE]"));
	TestEqual(TEXT("Compatible tool deltas assembled"), UUnrealAIResponseLibrary::GetResponseToolCalls(Chat.Result).Num(), 1);
	FUnrealAIResponseState Gemini = StreamState(*this, 3);
	Events.Reset();
	Frame(*this, Gemini, Events, TEXT(R"({"candidates":[{"content":{"role":"model","parts":[{"text":"Hello","thoughtSignature":"signed-text"}]},"groundingMetadata":{"fixture":true}}]})"));
	Frame(*this, Gemini, Events, TEXT(R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"weather","args":{"city":"Paris"}},"thoughtSignature":"signed-call"}]},"finishReason":"STOP"}]})"));
	UnrealAIResponseAdapters::Normalize(Gemini, true);
	TestEqual(TEXT("Gemini visible text"), UUnrealAIResponseLibrary::GetResponseText(Gemini.Result.Response), FString(TEXT("Hello")));
	TestEqual(TEXT("Gemini synthetic call ID stable"), UUnrealAIResponseLibrary::GetResponseToolCalls(Gemini.Result)[0].CallId, FString(TEXT("local_item_1")));
	TestTrue(TEXT("Gemini both signed source parts retained"), Gemini.Result.Response.Continuation.ItemsJson.Contains(TEXT("signed-text"))
		&& Gemini.Result.Response.Continuation.ItemsJson.Contains(TEXT("signed-call")));
	TestTrue(TEXT("Sidecar metadata retained"), Gemini.Result.Response.RawJson.Contains(TEXT("groundingMetadata")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIResponseSafetyTest, "UnrealAI.Responses.Safety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealAIResponseSafetyTest::RunTest(const FString& Parameters)
{
	using namespace UnrealAIResponseTests;
	for (int32 Protocol = 0; Protocol < 4; ++Protocol)
	{
		FUnrealAIResponseState State = StreamState(*this, Protocol);
		UnrealAIResponseAdapters::ParseResponse(200, TEXT("{}"), State);
		TestEqual(TEXT("Malformed success fails"), State.Result.Status, EUnrealAIResponseStatus::Failed);
		TestTrue(TEXT("Malformed response has no continuation"), State.Result.Response.Continuation.ItemsJson.IsEmpty());
	}
	FUnrealAIResponseState State = StreamState(*this, 0);
	UnrealAIResponseAdapters::ParseResponse(200,
		TEXT(R"({"id":"i","status":"incomplete","incomplete_details":{"reason":"max_output_tokens"},"output":[{"type":"function_call","call_id":"c","name":"weather","arguments":"{"}]})"), State);
	TestEqual(TEXT("Incomplete is not failure"), State.Result.Status, EUnrealAIResponseStatus::Incomplete);
	TestFalse(TEXT("Incomplete has no error"), State.Result.Error.bIsError);
	TestTrue(TEXT("Partial tools cannot execute"), UUnrealAIResponseLibrary::GetResponseToolCalls(State.Result).IsEmpty());
	State = StreamState(*this, 0);
	TArray<FUnrealAIResponseEvent> Events;
	FUnrealAIError Error;
	FUnrealAISseEvent Invalid;
	Invalid.Data = TEXT(R"({"type":"response.output_text.delta","output_index":9,"delta":"invalid"})");
	TestFalse(TEXT("Unknown item delta rejected"), UnrealAIResponseAdapters::ParseStreamEvent(Invalid, State, Events, Error));
	State = StreamState(*this, 0);
	if (!Frame(*this, State, Events, TEXT(R"({"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","call_id":"c","name":"weather","arguments":""}})")))
	{
		return false;
	}
	FObject Oversize = Object();
	Oversize->SetStringField(TEXT("type"), TEXT("response.function_call_arguments.delta"));
	Oversize->SetNumberField(TEXT("output_index"), 0);
	Oversize->SetStringField(TEXT("delta"), FString::ChrN(UnrealAIResponseAdapters::MaxToolArgumentBytes + 1, TEXT('a')));
	Invalid.Data = Serialize(Oversize);
	TestFalse(TEXT("Tool buffer bounded before append"), UnrealAIResponseAdapters::ParseStreamEvent(Invalid, State, Events, Error));
	TestEqual(TEXT("Argument overflow error"), Error.Code, FString(TEXT("tool_argument_overflow")));
	TestTrue(TEXT("Rejected bytes not retained"), State.Result.Response.Output[0].ArgumentsJson.IsEmpty());
	UUnrealAIClient* Client = NewObject<UUnrealAIClient>();
	int32 TerminalCount = 0;
	FUnrealAIRequestHandle Handle = Client->CreateResponse(Request(), FUnrealAIResponseNativeDelegate::CreateLambda(
		[this, &TerminalCount](const FUnrealAIResponseResult& Result)
		{
			++TerminalCount;
			TestTrue(TEXT("Preflight callback on game thread"), IsInGameThread());
			TestEqual(TEXT("Unconfigured request fails"), Result.Status, EUnrealAIResponseStatus::Failed);
		}));
	TestEqual(TEXT("Preflight terminal exactly once"), TerminalCount, 1);
	TestFalse(TEXT("Preflight returns no active handle"), Handle.IsValid());
	for (const FName Name : {FName(TEXT("CreateResponse")), FName(TEXT("StreamResponse"))})
	{
		UFunction* Function = UUnrealAIResponseAsyncAction::StaticClass()->FindFunctionByName(Name);
		TestNotNull(TEXT("Blueprint factory reflected"), Function);
		TestTrue(TEXT("Factory is callable"), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}
	TestNotNull(TEXT("Blueprint terminal status exposed"), FindFProperty<FProperty>(FUnrealAIResponseResult::StaticStruct(), TEXT("Status")));
	return true;
}

#endif
