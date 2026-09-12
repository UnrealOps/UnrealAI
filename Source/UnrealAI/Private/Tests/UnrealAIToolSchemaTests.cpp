// Copyright UnrealOps. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "OpenAI/UnrealAIOpenAIResponsesProtocol.h"
#include "OpenAI/UnrealAIOpenAIToolSchema.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
TSharedRef<FJsonObject> ReadSchemaFixture(const FString &Json)
{
	TSharedPtr<FJsonObject> Result;
	verify(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Result) && Result.IsValid());
	return Result.ToSharedRef();
}

const TCHAR *OptionalSchema = TEXT(R"({"type":"object","additionalProperties":false,
"properties":{"Mode":{"type":"string"},"Target":{"type":"string","format":"custom-id","pattern":"^[a-z]+$"},
"Rows":{"type":"array","items":{"type":"object","additionalProperties":false,
"properties":{"Key":{"type":"string"},"Value":{"type":"integer"}},"required":["Key"]}},
"Nullable":{"type":["string","null"]},"RequiredNullable":{"type":["string","null"]}},
"required":["Mode","Rows","RequiredNullable"]})");
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIToolSchemaProjectionTest, "UnrealAI.Native.StrictToolSchemaProjection",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIToolSchemaProjectionTest::RunTest(const FString &)
{
	const TSharedRef<FJsonObject> Original = ReadSchemaFixture(OptionalSchema);
	const TSharedRef<FJsonObject> Wire = ReadSchemaFixture(OptionalSchema);
	bool bOptional = false;
	TestTrue(TEXT("Inline tool schema is projected"),
				  UE::UnrealAI::OpenAI::Private::ProjectToolSchema(Wire, true, bOptional));
	TestTrue(TEXT("Optional normalization is selected"), bOptional);
	const TSharedPtr<FJsonObject> Properties = Wire->GetObjectField(TEXT("properties"));
	TestEqual(TEXT("All properties are required on the wire"), Wire->GetArrayField(TEXT("required")).Num(),
																				   Properties->Values.Num());
	const TArray<TSharedPtr<FJsonValue>> Alternatives =
		Properties->GetObjectField(TEXT("Target"))->GetArrayField(TEXT("anyOf"));
	TestFalse(TEXT("Custom format annotation is omitted"), Alternatives[0]->AsObject()->HasField(TEXT("format")));
	TestEqual(TEXT("Identifier pattern is retained"),
				   Alternatives[0]->AsObject()->GetStringField(TEXT("pattern")), FString(TEXT("^[a-z]+$")));
	TestTrue(
		TEXT("Original schema remains unchanged"),
			 Original->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("Target"))->HasField(TEXT("format")));
	const TSharedRef<FJsonObject> Arguments = ReadSchemaFixture(TEXT(R"({"Mode":null,"Target":null,
"Rows":[{"Key":"row","Value":null}],"Nullable":null,"RequiredNullable":null,"Unknown":null})"));
	TestTrue(TEXT("Synthetic nulls normalize"),
				  UE::UnrealAI::OpenAI::Private::NormalizeToolArguments(Original, Arguments));
	TestFalse(TEXT("Optional non-null target becomes absent"), Arguments->HasField(TEXT("Target")));
	TestFalse(TEXT("Nested optional value becomes absent"),
				   Arguments->GetArrayField(TEXT("Rows"))[0]->AsObject()->HasField(TEXT("Value")));
	TestTrue(TEXT("Originally nullable optional value remains"), Arguments->HasField(TEXT("Nullable")));
	TestTrue(TEXT("Required nullable value remains"), Arguments->HasField(TEXT("RequiredNullable")));
	TestTrue(TEXT("Invalid required null is preserved for caller rejection"), Arguments->HasField(TEXT("Mode")));
	TestTrue(TEXT("Unknown values are preserved for caller rejection"), Arguments->HasField(TEXT("Unknown")));
	const TSharedRef<FJsonObject> Known = ReadSchemaFixture(TEXT("{\"type\":\"string\",\"format\":\"uuid\"}"));
	TestTrue(TEXT("Supported format projects"),
				  UE::UnrealAI::OpenAI::Private::ProjectToolSchema(Known, true, bOptional));
	TestTrue(TEXT("Supported format is retained"), Known->HasField(TEXT("format")));
	const TSharedRef<FJsonObject> Compound = ReadSchemaFixture(TEXT(R"({"type":"object","properties":{
"Optional":{"anyOf":[{"type":"string"},{"type":"null"}]}},"additionalProperties":false})"));
	TestFalse(TEXT("Optional composition fails closed without a resolver"),
		UE::UnrealAI::OpenAI::Private::ProjectToolSchema(Compound, true, bOptional));
	const TSharedRef<FJsonObject> Reference = ReadSchemaFixture(TEXT(R"({"type":"object","properties":{
"Optional":{"$ref":"#/$defs/value"}},"$defs":{"value":{"type":"string"}},"additionalProperties":false})"));
	TestFalse(TEXT("Optional reference fails closed without narrowing its schema"),
		UE::UnrealAI::OpenAI::Private::ProjectToolSchema(Reference, true, bOptional));
	TestTrue(TEXT("Advisory schema retains optional references"),
		UE::UnrealAI::OpenAI::Private::ProjectToolSchema(Reference, false, bOptional));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIToolSchemaContinuationTest, "UnrealAI.Native.OptionalToolContinuation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIToolSchemaContinuationTest::RunTest(const FString &)
{
	FUnrealAIModelRequest Request;
	Request.RequestId.Value = FGuid::NewGuid();
	Request.ConnectionAlias = TEXT("tests.connection");
	Request.ModelId = TEXT("fixture-model");
	FUnrealAIModelMessage Message;
	FUnrealAIModelContentPart Part;
	Part.Text = TEXT("Exercise the offline optional tool.");
	Message.Content.Add(MoveTemp(Part));
	Request.InputMessages.Add(MoveTemp(Message));
	FUnrealAIModelToolDescriptor Tool;
	Tool.StableName = TEXT("tests.optional");
	Tool.Version = 1;
	Tool.InvocationName = TEXT("optional_v1");
	Tool.Description = TEXT("Offline optional tool.");
	Tool.InputJsonSchema = OptionalSchema;
	Tool.bStrict = true;
	Request.Tools.Add(Tool);
	const FString Arguments =
		TEXT("{\"Mode\":\"run\",\"Target\":null,\"Rows\":[],\"Nullable\":null,\"RequiredNullable\":null}");
	TArray<FUnrealAIModelEvent> Events;
	FUnrealAIOpenAIResponsesStreamDecoder Decoder(Request, [&Events](FUnrealAIModelEvent &&Event)
												  { Events.Add(MoveTemp(Event)); });
	const auto Push = [&Decoder](const FString &Json, FString &Error)
	{
		// SSE data records require one data-prefixed line; fixture formatting is not stream framing.
		const FString OneLine = Json.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(""));
		const FTCHARToUTF8 Utf8(*(TEXT("data: ") + OneLine + TEXT("\n\n")));
		return Decoder.PushBytes(MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()), Error);
	};
	const auto Encode = [](const TSharedRef<FJsonObject> &Object)
	{
		FString Json;
		verify(FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json)));
		return Json;
	};
	FString Error;
	TestTrue(TEXT("Response starts"), Push(TEXT(R"({"type":"response.created","sequence_number":0,
"response":{"id":"resp_optional"}})"),
												Error));
	TestTrue(TEXT("Tool starts"), Push(TEXT(R"({"type":"response.output_item.added","sequence_number":1,
"output_index":0,"item":{"type":"function_call","id":"fc_optional","call_id":"call_optional",
"name":"optional_v1","arguments":""}})"),
											Error));
	const TSharedRef<FJsonObject> Delta = ReadSchemaFixture(TEXT(R"({"type":"response.function_call_arguments.delta",
"sequence_number":2,"item_id":"fc_optional","output_index":0})"));
	Delta->SetStringField(TEXT("delta"), Arguments);
	TestTrue(TEXT("Raw provider arguments assemble"), Push(Encode(Delta), Error));
	const TSharedRef<FJsonObject> Item = ReadSchemaFixture(TEXT(R"({"type":"function_call","id":"fc_optional",
"call_id":"call_optional","name":"optional_v1"})"));
	Item->SetStringField(TEXT("arguments"), Arguments);
	const TSharedRef<FJsonObject> Completed =
		ReadSchemaFixture(TEXT(R"({"type":"response.completed","sequence_number":3,
"response":{"id":"resp_optional","output":[],"usage":{"input_tokens":5,"output_tokens":10,"total_tokens":15}}})"));
	Completed->GetObjectField(TEXT("response"))->SetArrayField(TEXT("output"), {MakeShared<FJsonValueObject>(Item)});
	TestTrue(TEXT("Complete provider response reconciles raw arguments"), Push(Encode(Completed), Error));
	TestTrue(TEXT("Decoder finishes"), Decoder.Finish(Error));
	const FUnrealAIModelEvent *Call = Events.FindByPredicate(
		[](const FUnrealAIModelEvent &Event) { return Event.Kind == EUnrealAIModelEventKind::ToolCallCompleted; });
	if (!TestNotNull(TEXT("Normalized tool call is published"), Call))
	{
		return false;
	}
	TestFalse(TEXT("Call omits synthetic target null"),
				   ReadSchemaFixture(Call->ToolCall.ArgumentsJson)->HasField(TEXT("Target")));
	TestFalse(
		TEXT("Raw optional argument deltas are not exposed"),
			 Events.ContainsByPredicate([](const FUnrealAIModelEvent &Event)
										{ return Event.Kind == EUnrealAIModelEventKind::ToolCallArgumentsDelta; }));
	if (!TestTrue(TEXT("Original provider continuation is retained"), Events.Last().Continuation.IsValid()))
	{
		return false;
	}
	FUnrealAIModelRequest Next = Request;
	Next.RequestId.Value = FGuid::NewGuid();
	Next.Continuation = Events.Last().Continuation;
	FUnrealAIModelToolOutput Output;
	Output.ProviderCallId = TEXT("call_optional");
	Output.OutputJson = TEXT("{\"ok\":true}");
	Next.ToolOutputs.Add(Output);
	FUnrealAIOpenAIResponsesWireRequest Wire;
	FUnrealAIModelError ModelError;
	if (!TestTrue(TEXT("Tool output continues the bound provider history"),
					   FUnrealAIOpenAIResponsesRequestBuilder::Build(Next, Wire, ModelError)))
	{
		return false;
	}
	const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR *>(Wire.BodyUtf8.GetData()), Wire.BodyUtf8.Num());
	const TSharedRef<FJsonObject> Body = ReadSchemaFixture(FString(Decoded.Length(), Decoded.Get()));
	bool bPreserved = false;
	for (const TSharedPtr<FJsonValue> &Value : Body->GetArrayField(TEXT("input")))
	{
		FString Type;
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object->TryGetStringField(TEXT("type"), Type) && Type == TEXT("function_call"))
		{
			bPreserved = Object->GetStringField(TEXT("arguments")) == Arguments;
		}
	}
	TestTrue(TEXT("Provider history retains exact original null-bearing arguments"), bPreserved);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIIncompleteReasonTest, "UnrealAI.Native.IncompleteReasonDisclosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIIncompleteReasonTest::RunTest(const FString &)
{
	struct FCase
	{
		const TCHAR *Reason;
		const TCHAR *Message;
	};
	const FCase Cases[] = {
		{TEXT("max_output_tokens"), TEXT("The provider reached its output-token limit before completing the response.")},
		{TEXT("content_filter"), TEXT("The provider stopped the response because of its content filter.")},
		{TEXT("private-provider-detail"), TEXT("The model provider could not complete the request.")},
		{TEXT(""), TEXT("The model provider could not complete the request.")}};
	for (const FCase &Case : Cases)
	{
		FUnrealAIModelRequest Request;
		Request.RequestId.Value = FGuid::NewGuid();
		Request.ConnectionAlias = TEXT("tests.connection");
		Request.ModelId = TEXT("fixture-model");
		TArray<FUnrealAIModelEvent> Events;
		FUnrealAIOpenAIResponsesStreamDecoder Decoder(Request,
			[&Events](FUnrealAIModelEvent &&Event) { Events.Add(MoveTemp(Event)); });
		const TSharedRef<FJsonObject> Event = ReadSchemaFixture(TEXT(R"({"type":"response.incomplete",
"sequence_number":0,"response":{"id":"resp_incomplete","incomplete_details":{}}})"));
		Event->GetObjectField(TEXT("response"))->GetObjectField(TEXT("incomplete_details"))
			->SetStringField(TEXT("reason"), Case.Reason);
		FString Json;
		FJsonSerializer::Serialize(Event, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
		const FTCHARToUTF8 Utf8(*(TEXT("data: ") + Json + TEXT("\n\n")));
		FString Error;
		TestFalse(TEXT("Incomplete response fails"),
			Decoder.PushBytes(MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()), Error));
		const TArray<FUnrealAIModelEvent> Terminals = Events.FilterByPredicate(
			[](const FUnrealAIModelEvent &Value) { return Value.IsTerminal(); });
		if (TestEqual(TEXT("One terminal failure"), Terminals.Num(), 1))
		{
			TestEqual(TEXT("Stable incomplete code"), Terminals[0].Error.Code, FName(TEXT("openai_response_incomplete")));
			TestEqual(TEXT("Only recognized public explanation"), Terminals[0].Error.UserMessage.ToString(), FString(Case.Message));
			TestFalse(TEXT("Diagnostic never discloses arbitrary reason"),
				Terminals[0].Error.DiagnosticMessage.Contains(TEXT("private-provider-detail")));
		}
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIMetadataOnlyCompletionTest, "UnrealAI.Native.MetadataOnlyCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIMetadataOnlyCompletionTest::RunTest(const FString &)
{
	for (int32 Case = 0; Case < 5; ++Case)
	{
		FUnrealAIModelRequest Request;
		Request.RequestId.Value = FGuid::NewGuid();
		Request.ConnectionAlias = TEXT("tests.connection");
		Request.ModelId = TEXT("fixture-model");
		FUnrealAIModelToolDescriptor Tool;
		Tool.StableName = TEXT("tests.echo");
		Tool.InvocationName = TEXT("echo_v1");
		Tool.Description = TEXT("Offline metadata-only completion fixture.");
		Tool.InputJsonSchema = TEXT("{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}");
		Request.Tools.Add(Tool);
		TArray<FUnrealAIModelEvent> Events;
		FUnrealAIOpenAIResponsesStreamDecoder Decoder(TEXT("openai.responses"),
			FUnrealAIOpenAIResponsesPublicFaultPolicy::OpenAI(), Request,
			[&Events](FUnrealAIModelEvent &&Event) { Events.Add(MoveTemp(Event)); }, {}, Case != 0);
		const auto Push = [&Decoder](FString Json, FString &Error)
		{
			Json = Json.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(""));
			const FTCHARToUTF8 Utf8(*(TEXT("data: ") + Json + TEXT("\n\n")));
			return Decoder.PushBytes(MakeArrayView(reinterpret_cast<const uint8 *>(Utf8.Get()), Utf8.Length()), Error);
		};
		FString Error;
		TestTrue(TEXT("Metadata fixture response starts"), Push(TEXT(R"({"type":"response.created",
"sequence_number":0,"response":{"id":"resp_metadata"}})"), Error));
		TestTrue(TEXT("Tool lifecycle starts"), Push(TEXT(R"({"type":"response.output_item.added",
"sequence_number":1,"output_index":0,"item":{"type":"function_call","id":"fc_metadata",
"call_id":"call_metadata","name":"echo_v1","arguments":""}})"), Error));
		TestTrue(TEXT("Tool arguments complete"), Push(TEXT(R"({"type":"response.function_call_arguments.done",
"sequence_number":2,"output_index":0,"item_id":"fc_metadata","arguments":"{}"})"), Error));
		if (Case != 2)
		{
			TestTrue(TEXT("Full streamed item completes"), Push(TEXT(R"({"type":"response.output_item.done",
"sequence_number":3,"output_index":0,"item":{"type":"function_call","id":"fc_metadata",
"call_id":"call_metadata","name":"echo_v1","arguments":"{}"}})"), Error));
		}
		if (Case != 4)
		{
			const FString Output = Case == 3
				? TEXT(R"([{"type":"function_call","id":"fc_changed","call_id":"call_metadata","name":"echo_v1","arguments":"{}"}])")
				: TEXT("[]");
			const FString Completed = TEXT("{\"type\":\"response.completed\",\"sequence_number\":4,\"response\":{")
				TEXT("\"id\":\"resp_metadata\",\"output\":") + Output +
				TEXT(",\"usage\":{\"input_tokens\":10,\"output_tokens\":5,\"total_tokens\":15}}}");
			TestEqual(TEXT("Only explicit metadata policy with full matching items completes"),
				Push(Completed, Error), Case == 1);
		}
		TestEqual(TEXT("A provider terminal is mandatory"), Decoder.Finish(Error), Case == 1);
		const TArray<FUnrealAIModelEvent> Terminals = Events.FilterByPredicate(
			[](const FUnrealAIModelEvent &Event) { return Event.IsTerminal(); });
		if (TestEqual(TEXT("One terminal per metadata completion case"), Terminals.Num(), 1))
		{
			TestEqual(TEXT("Only the valid opted-in stream authorizes completed calls"),
				Terminals[0].Kind == EUnrealAIModelEventKind::Completed, Case == 1);
			TestEqual(TEXT("Only valid completion retains continuation"), Terminals[0].Continuation.IsValid(), Case == 1);
		}
	}
	return true;
}
#endif
