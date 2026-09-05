#include "UnrealAIResponseAdapter.h"

#include "GenericPlatform/GenericPlatformHttp.h"
#include "Misc/SecureHash.h"
#include "UnrealAIResponseJson.h"

namespace UnrealAIResponseRequestPrivate
{
	using namespace UnrealAIResponseJson;

	FString BaseUrl(const FUnrealAIProviderConfig& Config)
	{
		FString Url = Config.BaseUrl.TrimStartAndEnd();
		while (Url.EndsWith(TEXT("/")))
		{
			Url.LeftChopInline(1);
		}
		return Url;
	}

	bool NativeResponses(const FUnrealAIProviderConfig& Config)
	{
		return Config.ResponseApi == EUnrealAIResponseApi::OpenAIResponses
			|| (Config.ResponseApi == EUnrealAIResponseApi::ProviderDefault
				&& Config.Api == EUnrealAIProviderApi::OpenAICompatibleChatCompletions
				&& Config.Name == TEXT("OpenAI")
				&& BaseUrl(Config).Equals(TEXT("https://api.openai.com/v1"), ESearchCase::IgnoreCase));
	}

	bool AddInput(const FUnrealAIResponseItem& Item, const FUnrealAIResponseContext& Context,
		FArray& Input, FUnrealAIError& Error)
	{
		const bool bAnthropic = Context.Api == EUnrealAIProviderApi::AnthropicMessages && !Context.bNativeResponses;
		const bool bGemini = Context.Api == EUnrealAIProviderApi::GeminiGenerateContent && !Context.bNativeResponses;
		FObject Entry = Object();
		if (Item.Type == EUnrealAIResponseItemType::ToolResult)
		{
			FString ToolOutput = Item.Output;
			if (Item.bToolError && !bAnthropic && !bGemini)
			{
				FObject Failure = Object();
				Failure->SetStringField(TEXT("error"), Item.Output);
				ToolOutput = Serialize(Failure);
			}
			if (Item.CallId.IsEmpty() || Item.ToolName.IsEmpty())
			{
				return Fail(Error, TEXT("Tool results require a call ID and tool name. Use MakeToolResult."));
			}
			if (Context.bNativeResponses)
			{
				Entry->SetStringField(TEXT("type"), TEXT("function_call_output"));
				Entry->SetStringField(TEXT("call_id"), Item.CallId);
				Entry->SetStringField(TEXT("output"), ToolOutput);
			}
			else if (bAnthropic || bGemini)
			{
				FObject Block = Object();
				if (bAnthropic)
				{
					Block->SetStringField(TEXT("type"), TEXT("tool_result"));
					Block->SetStringField(TEXT("tool_use_id"), Item.CallId);
					Block->SetStringField(TEXT("content"), Item.Output);
					Block->SetBoolField(TEXT("is_error"), Item.bToolError);
				}
				else
				{
					FObject Result = Object();
					Result->SetStringField(TEXT("name"), Item.ToolName);
					FObject OriginalCall = Child(Parse(Item.RawJson), TEXT("functionCall"));
					if (!String(OriginalCall, TEXT("id")).IsEmpty())
					{
						Result->SetStringField(TEXT("id"), Item.CallId);
					}
					FObject Output = Parse(Item.Output);
					if (!Output || Item.bToolError)
					{
						Output = Object();
						Output->SetStringField(Item.bToolError ? TEXT("error") : TEXT("result"), Item.Output);
					}
					Result->SetObjectField(TEXT("response"), Output);
					Block->SetObjectField(TEXT("functionResponse"), Result);
				}
				// Adjacent results belong to one user message (required for parallel tool use).
				const TCHAR* PartsKey = bAnthropic ? TEXT("content") : TEXT("parts");
				FObject Last = Input.IsEmpty() ? FObject() : AsObject(Input.Last());
				FArray LastParts = Array(Last, PartsKey);
				if (String(Last, TEXT("role")) == TEXT("user") && !LastParts.IsEmpty())
				{
					FObject First = AsObject(LastParts[0]);
					if (String(First, TEXT("type")) == TEXT("tool_result") || Child(First, TEXT("functionResponse")))
					{
						LastParts.Add(Value(Block));
						Last->SetArrayField(PartsKey, LastParts);
						return true;
					}
				}
				Entry->SetStringField(TEXT("role"), TEXT("user"));
				Entry->SetArrayField(PartsKey, {Value(Block)});
			}
			else
			{
				Entry->SetStringField(TEXT("role"), TEXT("tool"));
				Entry->SetStringField(TEXT("tool_call_id"), Item.CallId);
				Entry->SetStringField(TEXT("content"), ToolOutput);
			}
		}
		else if (Item.Type == EUnrealAIResponseItemType::Message)
		{
			if (Item.Role != EUnrealAIMessageRole::User && Item.Role != EUnrealAIMessageRole::Assistant)
			{
				return Fail(Error, TEXT("Response input messages use user or assistant roles. Set Instructions for system guidance."));
			}
			Entry->SetStringField(TEXT("role"), bGemini && Item.Role == EUnrealAIMessageRole::Assistant
				? TEXT("model") : Role(Item.Role));
			FArray Parts;
			for (const FUnrealAIResponsePart& Part : Item.Content)
			{
				if (Part.Type != EUnrealAIResponsePartType::Text)
				{
					return Fail(Error, TEXT("Only text input parts are supported. Use continuation to replay provider output."));
				}
				FObject Block = Object();
				if (!bGemini)
				{
					Block->SetStringField(TEXT("type"), Context.bNativeResponses
						? (Item.Role == EUnrealAIMessageRole::Assistant ? TEXT("output_text") : TEXT("input_text"))
						: TEXT("text"));
				}
				Block->SetStringField(TEXT("text"), Part.Text);
				Parts.Add(Value(Block));
			}
			if (Parts.IsEmpty())
			{
				return Fail(Error, TEXT("A response input message must contain text parts."));
			}
			Entry->SetArrayField(bGemini ? TEXT("parts") : TEXT("content"), Parts);
		}
		else
		{
			return Fail(Error, TEXT("Tool calls and provider data must be replayed through BuildContinuationRequest."));
		}
		Input.Add(Value(Entry));
		return true;
	}

	bool AddExtensions(const FString& Json, const FObject& Payload, FUnrealAIError& Error)
	{
		if (Json.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		FObject Extensions = Parse(Json);
		if (!Extensions)
		{
			return Fail(Error, TEXT("AdditionalParametersJson must be a JSON object."));
		}
		const TSet<FString> Reserved = {
			TEXT("model"), TEXT("input"), TEXT("messages"), TEXT("contents"), TEXT("instructions"),
			TEXT("system"), TEXT("systemInstruction"), TEXT("tools"), TEXT("tool_choice"), TEXT("toolConfig"),
			TEXT("stream"), TEXT("store"), TEXT("previous_response_id"), TEXT("conversation"), TEXT("background"),
			TEXT("text"), TEXT("response_format"), TEXT("output_config"), TEXT("generationConfig"),
			TEXT("temperature"), TEXT("top_p"), TEXT("max_tokens"), TEXT("max_output_tokens"),
			TEXT("max_completion_tokens"), TEXT("n"), TEXT("include")
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Extensions->Values)
		{
			if (Reserved.Contains(Pair.Key))
			{
				return Fail(Error, TEXT("AdditionalParametersJson cannot override SDK-owned request fields."));
			}
			Payload->SetField(Pair.Key, Pair.Value);
		}
		return true;
	}
}

FUnrealAIResponseCapabilities UnrealAIResponseAdapters::Capabilities(const FUnrealAIProviderConfig& Config)
{
	FUnrealAIResponseCapabilities Result;
	Result.bUsesOpenAIResponses = UnrealAIResponseRequestPrivate::NativeResponses(Config);
	const bool bGemini = Config.Api == EUnrealAIProviderApi::GeminiGenerateContent && !Result.bUsesOpenAIResponses;
	const bool bAnthropic = Config.Api == EUnrealAIProviderApi::AnthropicMessages && !Result.bUsesOpenAIResponses;
	Result.bText = Result.bStreaming = Result.bTools = true;
	Result.bRequiredToolChoice = Result.bNamedToolChoice = true;
	Result.bStrictTools = !bGemini;
	Result.bJsonObject = !bAnthropic;
	Result.bJsonSchema = true;
	Result.bStoredContinuation = Result.bUsesOpenAIResponses;
	return Result;
}

bool UnrealAIResponseAdapters::BuildRequest(const FUnrealAIProviderConfig& Config,
	const FUnrealAIResponseRequest& Request, const FString& ApiKey, EUnrealAIRequestMode Mode,
	FUnrealAIHttpRequestData& OutHttp, FUnrealAIResponseContext& OutContext, FUnrealAIError& OutError)
{
	using namespace UnrealAIResponseJson;
	using namespace UnrealAIResponseRequestPrivate;
	OutError = FUnrealAIError();
	OutContext = FUnrealAIResponseContext();
	OutContext.Api = Config.Api;
	OutContext.bNativeResponses = NativeResponses(Config);
	OutContext.Model = Request.Model.IsEmpty() ? Config.DefaultModel : Request.Model;
	OutContext.bStore = Request.bStore;
	if (static_cast<uint8>(Config.Api) > static_cast<uint8>(EUnrealAIProviderApi::GeminiGenerateContent)
		|| static_cast<uint8>(Config.ResponseApi) > static_cast<uint8>(EUnrealAIResponseApi::OpenAIResponses)
		|| static_cast<uint8>(Request.ToolChoice) > static_cast<uint8>(EUnrealAIToolChoice::Named)
		|| static_cast<uint8>(Request.OutputFormat) > static_cast<uint8>(EUnrealAIResponseFormat::JsonSchema))
	{
		return Fail(OutError, TEXT("Invalid response configuration enum value."));
	}
	const FString Url = BaseUrl(Config);
	const bool bAnthropic = Config.Api == EUnrealAIProviderApi::AnthropicMessages && !OutContext.bNativeResponses;
	const bool bGemini = Config.Api == EUnrealAIProviderApi::GeminiGenerateContent && !OutContext.bNativeResponses;
	if (Url.IsEmpty() || OutContext.Model.IsEmpty())
	{
		return Fail(OutError, TEXT("A provider base URL and model are required."));
	}
	if (OutContext.bNativeResponses && Config.Api != EUnrealAIProviderApi::OpenAICompatibleChatCompletions)
	{
		return Fail(OutError, TEXT("OpenAI Responses requires an OpenAI-compatible provider configuration."));
	}
	OutContext.Binding = FMD5::HashAnsiString(*FString::Printf(TEXT("%s|%s|%d|%d|%s|%s|%s"),
		*Config.Name.ToString(), *Url, static_cast<int32>(Config.Api), OutContext.bNativeResponses ? 1 : 0,
		*OutContext.Model, *Config.OrganizationId, *Config.ProjectId));
	if ((!Request.History.Binding.IsEmpty() && Request.History.Binding != OutContext.Binding)
		|| (!Request.History.ItemsJson.IsEmpty() && Request.History.Binding.IsEmpty()))
	{
		return Fail(OutError, TEXT("Continuation belongs to a different provider, protocol, or model."));
	}
	if ((Request.bStore || !Request.PreviousResponseId.IsEmpty()) && !OutContext.bNativeResponses)
	{
		return Fail(OutError, TEXT("Stored continuation requires OpenAI Responses."));
	}
	if (!Request.PreviousResponseId.IsEmpty() && (!Request.bStore || !Request.History.ItemsJson.IsEmpty()))
	{
		return Fail(OutError, TEXT("Stored continuation requires Store and must not also replay local history."));
	}
	if ((Request.bUseMaxOutputTokens && Request.MaxOutputTokens < 1)
		|| (Request.bUseTemperature && (!FMath::IsFinite(Request.Temperature) || Request.Temperature < 0.0f))
		|| (Request.bUseTopP && (!FMath::IsFinite(Request.TopP) || Request.TopP < 0.0f || Request.TopP > 1.0f)))
	{
		return Fail(OutError, TEXT("Invalid sampling or output-token limit."));
	}
	FArray Input;
	if (!Request.History.ItemsJson.IsEmpty() && !ParseArray(Request.History.ItemsJson, Input))
	{
		return Fail(OutError, TEXT("Invalid continuation history."));
	}
	for (const FUnrealAIResponseItem& Item : Request.Input)
	{
		if (!AddInput(Item, OutContext, Input, OutError))
		{
			return false;
		}
	}
	if (Input.IsEmpty())
	{
		return Fail(OutError, TEXT("A response request requires input or local history."));
	}
	OutContext.InputJson = Serialize(Input);
	FObject Payload = Object();
	if (!bGemini)
	{
		Payload->SetStringField(TEXT("model"), OutContext.Model);
	}
	if (!Request.Instructions.IsEmpty())
	{
		if (OutContext.bNativeResponses || bAnthropic)
		{
			Payload->SetStringField(OutContext.bNativeResponses ? TEXT("instructions") : TEXT("system"), Request.Instructions);
		}
		else if (bGemini)
		{
			FObject TextPart = Object();
			TextPart->SetStringField(TEXT("text"), Request.Instructions);
			FObject Instruction = Object();
			Instruction->SetArrayField(TEXT("parts"), {Value(TextPart)});
			Payload->SetObjectField(TEXT("systemInstruction"), Instruction);
		}
		else
		{
			FObject Instruction = Object();
			Instruction->SetStringField(TEXT("role"), TEXT("system"));
			Instruction->SetStringField(TEXT("content"), Request.Instructions);
			Input.Insert(Value(Instruction), 0);
		}
	}
	Payload->SetArrayField(OutContext.bNativeResponses ? TEXT("input") : (bGemini ? TEXT("contents") : TEXT("messages")), Input);
	FObject Generation = bGemini ? Object() : Payload;
	if (Request.bUseTemperature)
	{
		Generation->SetNumberField(TEXT("temperature"), Request.Temperature);
	}
	if (Request.bUseTopP)
	{
		Generation->SetNumberField(bGemini ? TEXT("topP") : TEXT("top_p"), Request.TopP);
	}
	if (Request.bUseMaxOutputTokens || bAnthropic)
	{
		Generation->SetNumberField(bGemini ? TEXT("maxOutputTokens") :
			(bAnthropic ? TEXT("max_tokens") : (OutContext.bNativeResponses ? TEXT("max_output_tokens") : TEXT("max_completion_tokens"))),
			Request.bUseMaxOutputTokens ? Request.MaxOutputTokens : 1024);
	}
	TSet<FString> ToolNames;
	FArray Tools;
	for (const FUnrealAIToolDefinition& Tool : Request.Tools)
	{
		FObject Schema = Parse(Tool.ParametersJson);
		if (Tool.Name.IsEmpty() || ToolNames.Contains(Tool.Name) || !Schema)
		{
			return Fail(OutError, TEXT("Tools require unique nonempty names and JSON object parameter schemas."));
		}
		if (Tool.bStrict && bGemini)
		{
			return Fail(OutError, TEXT("Strict tool mode is not implemented for Gemini."));
		}
		ToolNames.Add(Tool.Name);
		FObject Definition = Object();
		Definition->SetStringField(TEXT("name"), Tool.Name);
		Definition->SetStringField(TEXT("description"), Tool.Description);
		Definition->SetObjectField(bAnthropic ? TEXT("input_schema") : (bGemini ? TEXT("parametersJsonSchema") : TEXT("parameters")), Schema);
		if (!bGemini)
		{
			Definition->SetBoolField(TEXT("strict"), Tool.bStrict);
		}
		if (OutContext.bNativeResponses)
		{
			Definition->SetStringField(TEXT("type"), TEXT("function"));
		}
		else if (!bAnthropic && !bGemini)
		{
			FObject Wrapper = Object();
			Wrapper->SetStringField(TEXT("type"), TEXT("function"));
			Wrapper->SetObjectField(TEXT("function"), Definition);
			Definition = Wrapper;
		}
		Tools.Add(Value(Definition));
	}
	if ((Request.ToolChoice == EUnrealAIToolChoice::Required && Tools.IsEmpty())
		|| (Request.ToolChoice == EUnrealAIToolChoice::Named && !ToolNames.Contains(Request.NamedTool)))
	{
		return Fail(OutError, TEXT("Required or named tool selection must reference declared tools."));
	}
	if (!Tools.IsEmpty())
	{
		if (bGemini)
		{
			FObject Group = Object();
			Group->SetArrayField(TEXT("functionDeclarations"), Tools);
			Payload->SetArrayField(TEXT("tools"), {Value(Group)});
		}
		else
		{
			Payload->SetArrayField(TEXT("tools"), Tools);
		}
		FObject Choice = Object();
		const bool bNamed = Request.ToolChoice == EUnrealAIToolChoice::Named;
		if (bAnthropic)
		{
			Choice->SetStringField(TEXT("type"), bNamed ? TEXT("tool") :
				(Request.ToolChoice == EUnrealAIToolChoice::Required ? TEXT("any") :
				(Request.ToolChoice == EUnrealAIToolChoice::None ? TEXT("none") : TEXT("auto"))));
			if (bNamed)
			{
				Choice->SetStringField(TEXT("name"), Request.NamedTool);
			}
			Payload->SetObjectField(TEXT("tool_choice"), Choice);
		}
		else if (bGemini)
		{
			Choice->SetStringField(TEXT("mode"), Request.ToolChoice == EUnrealAIToolChoice::None ? TEXT("NONE") :
				(Request.ToolChoice == EUnrealAIToolChoice::Auto ? TEXT("AUTO") : TEXT("ANY")));
			if (bNamed)
			{
				Choice->SetArrayField(TEXT("allowedFunctionNames"), {MakeShared<FJsonValueString>(Request.NamedTool)});
			}
			FObject ConfigObject = Object();
			ConfigObject->SetObjectField(TEXT("functionCallingConfig"), Choice);
			Payload->SetObjectField(TEXT("toolConfig"), ConfigObject);
		}
		else if (bNamed)
		{
			Choice->SetStringField(TEXT("type"), TEXT("function"));
			if (OutContext.bNativeResponses)
			{
				Choice->SetStringField(TEXT("name"), Request.NamedTool);
			}
			else
			{
				FObject Function = Object();
				Function->SetStringField(TEXT("name"), Request.NamedTool);
				Choice->SetObjectField(TEXT("function"), Function);
			}
			Payload->SetObjectField(TEXT("tool_choice"), Choice);
		}
		else
		{
			Payload->SetStringField(TEXT("tool_choice"), Request.ToolChoice == EUnrealAIToolChoice::Required ? TEXT("required") :
				(Request.ToolChoice == EUnrealAIToolChoice::None ? TEXT("none") : TEXT("auto")));
		}
	}
	if (Request.OutputFormat != EUnrealAIResponseFormat::Text)
	{
		const bool bSchema = Request.OutputFormat == EUnrealAIResponseFormat::JsonSchema;
		FObject Schema = bSchema ? Parse(Request.OutputSchemaJson) : FObject();
		if ((bSchema && !Schema) || (!bSchema && bAnthropic))
		{
			return Fail(OutError, TEXT("JSON Schema requires an object schema; Anthropic does not support schema-free JSON-object mode."));
		}
		FObject Format = Object();
		Format->SetStringField(TEXT("type"), bSchema ? TEXT("json_schema") : TEXT("json_object"));
		if (bSchema)
		{
			Format->SetObjectField(TEXT("schema"), Schema);
			if (!bAnthropic && !bGemini)
			{
				Format->SetStringField(TEXT("name"), Request.OutputSchemaName);
				Format->SetBoolField(TEXT("strict"), Request.bStrictOutput);
			}
		}
		if (bGemini)
		{
			Generation->SetStringField(TEXT("responseMimeType"), TEXT("application/json"));
			if (bSchema)
			{
				Generation->SetObjectField(TEXT("responseJsonSchema"), Schema);
			}
		}
		else if (OutContext.bNativeResponses || bAnthropic)
		{
			FObject Output = Object();
			Output->SetObjectField(TEXT("format"), Format);
			Payload->SetObjectField(bAnthropic ? TEXT("output_config") : TEXT("text"), Output);
		}
		else
		{
			if (bSchema)
			{
				Format->RemoveField(TEXT("type"));
				FObject Wrapper = Object();
				Wrapper->SetStringField(TEXT("type"), TEXT("json_schema"));
				Wrapper->SetObjectField(TEXT("json_schema"), Format);
				Format = Wrapper;
			}
			Payload->SetObjectField(TEXT("response_format"), Format);
		}
	}
	if (bGemini && Generation->Values.Num() > 0)
	{
		Payload->SetObjectField(TEXT("generationConfig"), Generation);
	}
	if (OutContext.bNativeResponses)
	{
		Payload->SetBoolField(TEXT("store"), Request.bStore);
		if (!Request.PreviousResponseId.IsEmpty())
		{
			Payload->SetStringField(TEXT("previous_response_id"), Request.PreviousResponseId);
		}
		if (!Request.bStore)
		{
			Payload->SetArrayField(TEXT("include"), {MakeShared<FJsonValueString>(TEXT("reasoning.encrypted_content"))});
		}
	}
	if (!AddExtensions(Request.AdditionalParametersJson, Payload, OutError))
	{
		return false;
	}
	if (!bGemini)
	{
		Payload->SetBoolField(TEXT("stream"), Mode == EUnrealAIRequestMode::Stream);
	}
	OutHttp = FUnrealAIHttpRequestData();
	OutHttp.ResolvedModel = OutContext.Model;
	FString Path = OutContext.bNativeResponses ? TEXT("responses") : (bAnthropic ? TEXT("messages") : TEXT("chat/completions"));
	if (bGemini)
	{
		FString Model = OutContext.Model;
		Model.RemoveFromStart(TEXT("models/"));
		Path = FString::Printf(TEXT("models/%s:%s"), *FGenericPlatformHttp::UrlEncode(Model),
			Mode == EUnrealAIRequestMode::Stream ? TEXT("streamGenerateContent?alt=sse") : TEXT("generateContent"));
	}
	OutHttp.Url = Url + TEXT("/") + Path;
	OutHttp.Headers.Add(TEXT("Content-Type"), TEXT("application/json"));
	if (!ApiKey.IsEmpty())
	{
		OutHttp.Headers.Add(bAnthropic ? TEXT("x-api-key") : (bGemini ? TEXT("x-goog-api-key") : TEXT("Authorization")),
			bAnthropic || bGemini ? ApiKey : TEXT("Bearer ") + ApiKey);
	}
	if (bAnthropic)
	{
		OutHttp.Headers.Add(TEXT("anthropic-version"), TEXT("2023-06-01"));
	}
	if (!bAnthropic && !bGemini)
	{
		if (!Config.OrganizationId.IsEmpty())
		{
			OutHttp.Headers.Add(TEXT("OpenAI-Organization"), Config.OrganizationId);
		}
		if (!Config.ProjectId.IsEmpty())
		{
			OutHttp.Headers.Add(TEXT("OpenAI-Project"), Config.ProjectId);
		}
	}
	OutHttp.Headers.Append(Config.AdditionalHeaders);
	OutHttp.Headers.Add(TEXT("Accept"), Mode == EUnrealAIRequestMode::Stream ? TEXT("text/event-stream") : TEXT("application/json"));
	if (Mode == EUnrealAIRequestMode::Stream)
	{
		OutHttp.Headers.Add(TEXT("Cache-Control"), TEXT("no-cache"));
	}
	OutHttp.Body = Serialize(Payload);
	return true;
}
