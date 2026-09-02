#include "UnrealAIProviderAdapter.h"

#include "GenericPlatform/GenericPlatformHttp.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UnrealAIProviderAdapterPrivate
{
	constexpr int32 DefaultAnthropicMaxTokens = 1024;

	FString RoleToOpenAIString(EUnrealAIMessageRole Role)
	{
		switch (Role)
		{
		case EUnrealAIMessageRole::System:
			return TEXT("system");
		case EUnrealAIMessageRole::Developer:
			return TEXT("developer");
		case EUnrealAIMessageRole::Assistant:
			return TEXT("assistant");
		case EUnrealAIMessageRole::Tool:
			return TEXT("tool");
		case EUnrealAIMessageRole::User:
		default:
			return TEXT("user");
		}
	}

	bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		if (Json.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("JSON string is empty.");
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid JSON object: %s"), *Json);
			return false;
		}

		return true;
	}

	bool ParseJsonValue(const FString& Json, TSharedPtr<FJsonValue>& OutValue, FString& OutError)
	{
		if (Json.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("JSON value is empty.");
			return false;
		}

		TSharedPtr<FJsonObject> Wrapper;
		const FString WrappedJson = FString::Printf(TEXT("{\"value\":%s}"), *Json);
		if (!ParseJsonObject(WrappedJson, Wrapper, OutError))
		{
			return false;
		}

		OutValue = Wrapper->TryGetField(TEXT("value"));
		if (!OutValue.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid JSON value: %s"), *Json);
			return false;
		}

		return true;
	}

	bool SerializeJsonObject(const TSharedRef<FJsonObject>& Object, FString& OutJson)
	{
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		return FJsonSerializer::Serialize(Object, Writer);
	}

	void MergeJsonObject(const TSharedRef<FJsonObject>& Target, const TSharedRef<FJsonObject>& Source)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
		{
			Target->SetField(Pair.Key, Pair.Value);
		}
	}

	FString JsonObjectToString(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return FString();
		}

		FString OutJson;
		SerializeJsonObject(Object.ToSharedRef(), OutJson);
		return OutJson;
	}

	FString BuildEndpointUrl(const FString& BaseUrlValue, const FString& PathValue)
	{
		FString BaseUrl = BaseUrlValue.TrimStartAndEnd();
		while (BaseUrl.EndsWith(TEXT("/")))
		{
			BaseUrl.LeftChopInline(1);
		}

		FString Path = PathValue;
		while (Path.StartsWith(TEXT("/")))
		{
			Path.RightChopInline(1);
		}

		return FString::Printf(TEXT("%s/%s"), *BaseUrl, *Path);
	}

	void AddCommonHeaders(FUnrealAIHttpRequestData& RequestData)
	{
		RequestData.Headers.Add(TEXT("Content-Type"), TEXT("application/json"));
		RequestData.Headers.Add(TEXT("Accept"), TEXT("application/json"));
	}

	void AddAdditionalHeaders(const FUnrealAIProviderConfig& ProviderConfig, FUnrealAIHttpRequestData& RequestData)
	{
		for (const TPair<FString, FString>& Header : ProviderConfig.AdditionalHeaders)
		{
			if (!Header.Key.IsEmpty())
			{
				RequestData.Headers.Add(Header.Key, Header.Value);
			}
		}
	}

	bool ValidateCommonRequest(
		const FUnrealAIProviderConfig& ProviderConfig,
		const FUnrealAIChatRequest& Request,
		FString& OutModel,
		FUnrealAIError& OutError)
	{
		if (ProviderConfig.BaseUrl.TrimStartAndEnd().IsEmpty())
		{
			OutError = UnrealAIProviderAdapters::MakeError(TEXT("The provider base URL is empty."));
			return false;
		}

		OutModel = Request.Model.IsEmpty() ? ProviderConfig.DefaultModel : Request.Model;
		if (OutModel.IsEmpty())
		{
			OutError = UnrealAIProviderAdapters::MakeError(
				TEXT("No model was specified and the provider profile does not define a default model."));
			return false;
		}

		if (Request.Messages.Num() == 0)
		{
			OutError = UnrealAIProviderAdapters::MakeError(TEXT("Chat completion request must contain at least one message."));
			return false;
		}

		return true;
	}

	bool MergeAdditionalFields(
		const FString& AdditionalFieldsJson,
		const TSharedRef<FJsonObject>& Target,
		const FString& Context,
		FUnrealAIError& OutError)
	{
		if (AdditionalFieldsJson.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}

		TSharedPtr<FJsonObject> AdditionalFields;
		FString ParseError;
		if (!ParseJsonObject(AdditionalFieldsJson, AdditionalFields, ParseError))
		{
			OutError = UnrealAIProviderAdapters::MakeError(
				FString::Printf(TEXT("Invalid AdditionalFieldsJson for %s: %s"), *Context, *ParseError));
			return false;
		}

		MergeJsonObject(Target, AdditionalFields.ToSharedRef());
		return true;
	}

	bool MergeAdditionalParameters(
		const FString& AdditionalParametersJson,
		const TSharedRef<FJsonObject>& Target,
		FUnrealAIError& OutError)
	{
		if (AdditionalParametersJson.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}

		TSharedPtr<FJsonObject> AdditionalParameters;
		FString ParseError;
		if (!ParseJsonObject(AdditionalParametersJson, AdditionalParameters, ParseError))
		{
			OutError = UnrealAIProviderAdapters::MakeError(
				FString::Printf(TEXT("Invalid AdditionalParametersJson: %s"), *ParseError));
			return false;
		}

		MergeJsonObject(Target, AdditionalParameters.ToSharedRef());
		return true;
	}

	FUnrealAIError ParseApiError(int32 HttpStatus, const FString& RawJson)
	{
		FUnrealAIError Error = UnrealAIProviderAdapters::MakeError(
			TEXT("The provider returned an error."), HttpStatus, RawJson);

		TSharedPtr<FJsonObject> RootObject;
		FString ParseError;
		if (!ParseJsonObject(RawJson, RootObject, ParseError))
		{
			Error.Message = RawJson.IsEmpty()
				? FString::Printf(TEXT("HTTP request failed with status %d."), HttpStatus)
				: RawJson;
			return Error;
		}

		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (RootObject->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && ErrorObject->IsValid())
		{
			(*ErrorObject)->TryGetStringField(TEXT("message"), Error.Message);
			(*ErrorObject)->TryGetStringField(TEXT("type"), Error.Type);
			if (Error.Type.IsEmpty())
			{
				(*ErrorObject)->TryGetStringField(TEXT("status"), Error.Type);
			}
			(*ErrorObject)->TryGetStringField(TEXT("param"), Error.Param);

			FString Code;
			if ((*ErrorObject)->TryGetStringField(TEXT("code"), Code))
			{
				Error.Code = Code;
			}
			else
			{
				int32 NumericCode = 0;
				if ((*ErrorObject)->TryGetNumberField(TEXT("code"), NumericCode))
				{
					Error.Code = FString::FromInt(NumericCode);
				}
			}
		}
		else
		{
			RootObject->TryGetStringField(TEXT("message"), Error.Message);
			RootObject->TryGetStringField(TEXT("type"), Error.Type);
		}

		if (Error.Message.IsEmpty())
		{
			Error.Message = FString::Printf(TEXT("HTTP request failed with status %d."), HttpStatus);
		}

		return Error;
	}

	bool ParseSuccessfulObject(
		int32 HttpStatus,
		const FString& RawJson,
		TSharedPtr<FJsonObject>& OutRootObject,
		FUnrealAIError& OutError)
	{
		if (HttpStatus < 200 || HttpStatus >= 300)
		{
			OutError = ParseApiError(HttpStatus, RawJson);
			return false;
		}

		FString ParseError;
		if (!ParseJsonObject(RawJson, OutRootObject, ParseError))
		{
			OutError = UnrealAIProviderAdapters::MakeError(
				FString::Printf(TEXT("Provider returned invalid JSON: %s"), *ParseError), HttpStatus, RawJson);
			return false;
		}

		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (OutRootObject->TryGetObjectField(TEXT("error"), ErrorObject))
		{
			OutError = ParseApiError(HttpStatus, RawJson);
			return false;
		}

		return true;
	}

	class FOpenAIProviderAdapter final : public IUnrealAIProviderAdapter
	{
	public:
		virtual bool BuildRequest(
			const FUnrealAIProviderConfig& ProviderConfig,
			const FUnrealAIChatRequest& Request,
			const FString& ApiKey,
			FUnrealAIHttpRequestData& OutRequest,
			FUnrealAIError& OutError) const override
		{
			FString Model;
			if (!ValidateCommonRequest(ProviderConfig, Request, Model, OutError))
			{
				return false;
			}

			const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("model"), Model);

			TArray<TSharedPtr<FJsonValue>> Messages;
			for (const FUnrealAIChatMessage& Message : Request.Messages)
			{
				const TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
				MessageObject->SetStringField(TEXT("role"), RoleToOpenAIString(Message.Role));

				if (!Message.ContentJson.TrimStartAndEnd().IsEmpty())
				{
					TSharedPtr<FJsonValue> ContentValue;
					FString ParseError;
					if (!ParseJsonValue(Message.ContentJson, ContentValue, ParseError))
					{
						OutError = UnrealAIProviderAdapters::MakeError(
							FString::Printf(TEXT("Invalid ContentJson for message: %s"), *ParseError));
						return false;
					}
					MessageObject->SetField(TEXT("content"), ContentValue);
				}
				else
				{
					MessageObject->SetStringField(TEXT("content"), Message.Content);
				}

				if (!Message.Name.IsEmpty())
				{
					MessageObject->SetStringField(TEXT("name"), Message.Name);
				}
				if (!Message.ToolCallId.IsEmpty())
				{
					MessageObject->SetStringField(TEXT("tool_call_id"), Message.ToolCallId);
				}
				if (!MergeAdditionalFields(Message.AdditionalFieldsJson, MessageObject, TEXT("message"), OutError))
				{
					return false;
				}

				Messages.Add(MakeShared<FJsonValueObject>(MessageObject));
			}
			Payload->SetArrayField(TEXT("messages"), Messages);

			if (Request.bUseTemperature)
			{
				Payload->SetNumberField(TEXT("temperature"), Request.Temperature);
			}
			if (Request.bUseTopP)
			{
				Payload->SetNumberField(TEXT("top_p"), Request.TopP);
			}
			if (Request.bUseMaxCompletionTokens)
			{
				Payload->SetNumberField(TEXT("max_completion_tokens"), Request.MaxCompletionTokens);
			}
			if (Request.bUseLegacyMaxTokens)
			{
				Payload->SetNumberField(TEXT("max_tokens"), Request.MaxTokens);
			}
			if (Request.NumChoices > 1)
			{
				Payload->SetNumberField(TEXT("n"), Request.NumChoices);
			}
			if (Request.StopSequences.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> StopValues;
				for (const FString& StopSequence : Request.StopSequences)
				{
					StopValues.Add(MakeShared<FJsonValueString>(StopSequence));
				}
				Payload->SetArrayField(TEXT("stop"), StopValues);
			}
			if (!Request.User.IsEmpty())
			{
				Payload->SetStringField(TEXT("user"), Request.User);
			}
			if (!Request.ResponseFormatJson.TrimStartAndEnd().IsEmpty())
			{
				TSharedPtr<FJsonObject> ResponseFormat;
				FString ParseError;
				if (!ParseJsonObject(Request.ResponseFormatJson, ResponseFormat, ParseError))
				{
					OutError = UnrealAIProviderAdapters::MakeError(
						FString::Printf(TEXT("Invalid ResponseFormatJson: %s"), *ParseError));
					return false;
				}
				Payload->SetObjectField(TEXT("response_format"), ResponseFormat);
			}
			if (!MergeAdditionalParameters(Request.AdditionalParametersJson, Payload, OutError))
			{
				return false;
			}

			OutRequest = FUnrealAIHttpRequestData();
			OutRequest.Url = BuildEndpointUrl(ProviderConfig.BaseUrl, TEXT("chat/completions"));
			OutRequest.ResolvedModel = Model;
			AddCommonHeaders(OutRequest);
			if (!ApiKey.IsEmpty())
			{
				OutRequest.Headers.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
			}
			if (!ProviderConfig.OrganizationId.IsEmpty())
			{
				OutRequest.Headers.Add(TEXT("OpenAI-Organization"), ProviderConfig.OrganizationId);
			}
			if (!ProviderConfig.ProjectId.IsEmpty())
			{
				OutRequest.Headers.Add(TEXT("OpenAI-Project"), ProviderConfig.ProjectId);
			}
			AddAdditionalHeaders(ProviderConfig, OutRequest);

			if (!SerializeJsonObject(Payload, OutRequest.Body))
			{
				OutError = UnrealAIProviderAdapters::MakeError(TEXT("Failed to serialize chat completion request."));
				return false;
			}
			return true;
		}

		virtual void ParseResponse(
			const FString& ResolvedModel,
			int32 HttpStatus,
			const FString& RawJson,
			FUnrealAIChatResponse& OutResponse,
			FUnrealAIError& OutError) const override
		{
			OutResponse = FUnrealAIChatResponse();
			OutResponse.RawJson = RawJson;
			OutError = FUnrealAIError();

			TSharedPtr<FJsonObject> RootObject;
			if (!ParseSuccessfulObject(HttpStatus, RawJson, RootObject, OutError))
			{
				return;
			}

			RootObject->TryGetStringField(TEXT("id"), OutResponse.Id);
			RootObject->TryGetStringField(TEXT("object"), OutResponse.Object);
			if (!RootObject->TryGetStringField(TEXT("model"), OutResponse.Model))
			{
				OutResponse.Model = ResolvedModel;
			}
			RootObject->TryGetNumberField(TEXT("created"), OutResponse.CreatedUnixTime);

			const TSharedPtr<FJsonObject>* UsageObject = nullptr;
			if (RootObject->TryGetObjectField(TEXT("usage"), UsageObject) && UsageObject && UsageObject->IsValid())
			{
				(*UsageObject)->TryGetNumberField(TEXT("prompt_tokens"), OutResponse.Usage.PromptTokens);
				(*UsageObject)->TryGetNumberField(TEXT("completion_tokens"), OutResponse.Usage.CompletionTokens);
				(*UsageObject)->TryGetNumberField(TEXT("total_tokens"), OutResponse.Usage.TotalTokens);
			}

			const TArray<TSharedPtr<FJsonValue>>* ChoicesArray = nullptr;
			if (RootObject->TryGetArrayField(TEXT("choices"), ChoicesArray) && ChoicesArray)
			{
				for (const TSharedPtr<FJsonValue>& ChoiceValue : *ChoicesArray)
				{
					if (!ChoiceValue.IsValid() || ChoiceValue->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> ChoiceObject = ChoiceValue->AsObject();
					FUnrealAIChatChoice Choice;
					ChoiceObject->TryGetNumberField(TEXT("index"), Choice.Index);
					ChoiceObject->TryGetStringField(TEXT("finish_reason"), Choice.FinishReason);

					const TSharedPtr<FJsonObject>* MessageObject = nullptr;
					if (ChoiceObject->TryGetObjectField(TEXT("message"), MessageObject) && MessageObject && MessageObject->IsValid())
					{
						(*MessageObject)->TryGetStringField(TEXT("role"), Choice.Role);
						(*MessageObject)->TryGetStringField(TEXT("content"), Choice.Content);
						Choice.RawMessageJson = JsonObjectToString(*MessageObject);
					}

					OutResponse.Choices.Add(Choice);
				}
			}
		}
	};

	class FAnthropicProviderAdapter final : public IUnrealAIProviderAdapter
	{
	public:
		virtual bool BuildRequest(
			const FUnrealAIProviderConfig& ProviderConfig,
			const FUnrealAIChatRequest& Request,
			const FString& ApiKey,
			FUnrealAIHttpRequestData& OutRequest,
			FUnrealAIError& OutError) const override
		{
			FString Model;
			if (!ValidateCommonRequest(ProviderConfig, Request, Model, OutError))
			{
				return false;
			}
			if (Request.NumChoices != 1)
			{
				OutError = UnrealAIProviderAdapters::MakeError(TEXT("Anthropic Messages supports one choice per request."));
				return false;
			}
			if (!Request.ResponseFormatJson.TrimStartAndEnd().IsEmpty())
			{
				OutError = UnrealAIProviderAdapters::MakeError(
					TEXT("ResponseFormatJson is OpenAI-specific. Use AdditionalParametersJson for Anthropic output_config."));
				return false;
			}

			const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("model"), Model);
			const int32 MaxTokens = Request.bUseMaxCompletionTokens
				? Request.MaxCompletionTokens
				: (Request.bUseLegacyMaxTokens ? Request.MaxTokens : DefaultAnthropicMaxTokens);
			Payload->SetNumberField(TEXT("max_tokens"), MaxTokens);

			TArray<TSharedPtr<FJsonValue>> SystemBlocks;
			TArray<TSharedPtr<FJsonValue>> Messages;
			for (const FUnrealAIChatMessage& Message : Request.Messages)
			{
				if (Message.Role == EUnrealAIMessageRole::System || Message.Role == EUnrealAIMessageRole::Developer)
				{
					if (!Message.AdditionalFieldsJson.TrimStartAndEnd().IsEmpty())
					{
						OutError = UnrealAIProviderAdapters::MakeError(
							TEXT("AdditionalFieldsJson is not supported on Anthropic system or developer messages."));
						return false;
					}

					if (!Message.ContentJson.TrimStartAndEnd().IsEmpty())
					{
						TSharedPtr<FJsonValue> ContentValue;
						FString ParseError;
						if (!ParseJsonValue(Message.ContentJson, ContentValue, ParseError))
						{
							OutError = UnrealAIProviderAdapters::MakeError(
								FString::Printf(TEXT("Invalid Anthropic system ContentJson: %s"), *ParseError));
							return false;
						}
						if (ContentValue->Type == EJson::Array)
						{
							SystemBlocks.Append(ContentValue->AsArray());
						}
						else
						{
							SystemBlocks.Add(ContentValue);
						}
					}
					else
					{
						const TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
						TextBlock->SetStringField(TEXT("type"), TEXT("text"));
						TextBlock->SetStringField(TEXT("text"), Message.Content);
						SystemBlocks.Add(MakeShared<FJsonValueObject>(TextBlock));
					}
					continue;
				}

				if (Message.Role == EUnrealAIMessageRole::Tool)
				{
					OutError = UnrealAIProviderAdapters::MakeError(
						TEXT("Tool-role messages are not normalized for Anthropic yet. Use provider-native raw JSON in a future tool adapter."));
					return false;
				}
				if (!Message.Name.IsEmpty() || !Message.ToolCallId.IsEmpty())
				{
					OutError = UnrealAIProviderAdapters::MakeError(
						TEXT("Message Name and ToolCallId are not supported by the core Anthropic chat adapter."));
					return false;
				}

				const TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
				MessageObject->SetStringField(
					TEXT("role"),
					Message.Role == EUnrealAIMessageRole::Assistant ? TEXT("assistant") : TEXT("user"));
				if (!Message.ContentJson.TrimStartAndEnd().IsEmpty())
				{
					TSharedPtr<FJsonValue> ContentValue;
					FString ParseError;
					if (!ParseJsonValue(Message.ContentJson, ContentValue, ParseError))
					{
						OutError = UnrealAIProviderAdapters::MakeError(
							FString::Printf(TEXT("Invalid Anthropic ContentJson: %s"), *ParseError));
						return false;
					}
					MessageObject->SetField(TEXT("content"), ContentValue);
				}
				else
				{
					MessageObject->SetStringField(TEXT("content"), Message.Content);
				}
				if (!MergeAdditionalFields(Message.AdditionalFieldsJson, MessageObject, TEXT("Anthropic message"), OutError))
				{
					return false;
				}
				Messages.Add(MakeShared<FJsonValueObject>(MessageObject));
			}

			if (Messages.Num() == 0)
			{
				OutError = UnrealAIProviderAdapters::MakeError(TEXT("Anthropic requires at least one user or assistant message."));
				return false;
			}

			Payload->SetArrayField(TEXT("messages"), Messages);
			if (SystemBlocks.Num() > 0)
			{
				Payload->SetArrayField(TEXT("system"), SystemBlocks);
			}
			if (Request.bUseTemperature)
			{
				Payload->SetNumberField(TEXT("temperature"), Request.Temperature);
			}
			if (Request.bUseTopP)
			{
				Payload->SetNumberField(TEXT("top_p"), Request.TopP);
			}
			if (Request.StopSequences.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> StopValues;
				for (const FString& StopSequence : Request.StopSequences)
				{
					StopValues.Add(MakeShared<FJsonValueString>(StopSequence));
				}
				Payload->SetArrayField(TEXT("stop_sequences"), StopValues);
			}
			if (!Request.User.IsEmpty())
			{
				const TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
				Metadata->SetStringField(TEXT("user_id"), Request.User);
				Payload->SetObjectField(TEXT("metadata"), Metadata);
			}
			if (!MergeAdditionalParameters(Request.AdditionalParametersJson, Payload, OutError))
			{
				return false;
			}

			OutRequest = FUnrealAIHttpRequestData();
			OutRequest.Url = BuildEndpointUrl(ProviderConfig.BaseUrl, TEXT("messages"));
			OutRequest.ResolvedModel = Model;
			AddCommonHeaders(OutRequest);
			OutRequest.Headers.Add(TEXT("anthropic-version"), TEXT("2023-06-01"));
			if (!ApiKey.IsEmpty())
			{
				OutRequest.Headers.Add(TEXT("x-api-key"), ApiKey);
			}
			AddAdditionalHeaders(ProviderConfig, OutRequest);

			if (!SerializeJsonObject(Payload, OutRequest.Body))
			{
				OutError = UnrealAIProviderAdapters::MakeError(TEXT("Failed to serialize Anthropic Messages request."));
				return false;
			}
			return true;
		}

		virtual void ParseResponse(
			const FString& ResolvedModel,
			int32 HttpStatus,
			const FString& RawJson,
			FUnrealAIChatResponse& OutResponse,
			FUnrealAIError& OutError) const override
		{
			OutResponse = FUnrealAIChatResponse();
			OutResponse.RawJson = RawJson;
			OutError = FUnrealAIError();

			TSharedPtr<FJsonObject> RootObject;
			if (!ParseSuccessfulObject(HttpStatus, RawJson, RootObject, OutError))
			{
				return;
			}

			RootObject->TryGetStringField(TEXT("id"), OutResponse.Id);
			RootObject->TryGetStringField(TEXT("type"), OutResponse.Object);
			if (!RootObject->TryGetStringField(TEXT("model"), OutResponse.Model))
			{
				OutResponse.Model = ResolvedModel;
			}

			FUnrealAIChatChoice Choice;
			Choice.Index = 0;
			RootObject->TryGetStringField(TEXT("role"), Choice.Role);
			RootObject->TryGetStringField(TEXT("stop_reason"), Choice.FinishReason);

			const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
			if (RootObject->TryGetArrayField(TEXT("content"), ContentArray) && ContentArray)
			{
				for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
				{
					if (!ContentValue.IsValid() || ContentValue->Type != EJson::Object)
					{
						continue;
					}
					const TSharedPtr<FJsonObject> ContentObject = ContentValue->AsObject();
					FString Type;
					FString Text;
					ContentObject->TryGetStringField(TEXT("type"), Type);
					if (Type == TEXT("text") && ContentObject->TryGetStringField(TEXT("text"), Text))
					{
						Choice.Content += Text;
					}
				}

				const TSharedRef<FJsonObject> RawMessage = MakeShared<FJsonObject>();
				RawMessage->SetStringField(TEXT("role"), Choice.Role);
				RawMessage->SetArrayField(TEXT("content"), *ContentArray);
				Choice.RawMessageJson = JsonObjectToString(RawMessage);
			}
			OutResponse.Choices.Add(Choice);

			const TSharedPtr<FJsonObject>* UsageObject = nullptr;
			if (RootObject->TryGetObjectField(TEXT("usage"), UsageObject) && UsageObject && UsageObject->IsValid())
			{
				(*UsageObject)->TryGetNumberField(TEXT("input_tokens"), OutResponse.Usage.PromptTokens);
				(*UsageObject)->TryGetNumberField(TEXT("output_tokens"), OutResponse.Usage.CompletionTokens);
				OutResponse.Usage.TotalTokens = OutResponse.Usage.PromptTokens + OutResponse.Usage.CompletionTokens;
			}
		}
	};

	class FGeminiProviderAdapter final : public IUnrealAIProviderAdapter
	{
	public:
		virtual bool BuildRequest(
			const FUnrealAIProviderConfig& ProviderConfig,
			const FUnrealAIChatRequest& Request,
			const FString& ApiKey,
			FUnrealAIHttpRequestData& OutRequest,
			FUnrealAIError& OutError) const override
		{
			FString Model;
			if (!ValidateCommonRequest(ProviderConfig, Request, Model, OutError))
			{
				return false;
			}
			if (Request.NumChoices != 1)
			{
				OutError = UnrealAIProviderAdapters::MakeError(TEXT("The core Gemini adapter supports one candidate per request."));
				return false;
			}
			if (!Request.ResponseFormatJson.TrimStartAndEnd().IsEmpty())
			{
				OutError = UnrealAIProviderAdapters::MakeError(
					TEXT("ResponseFormatJson is OpenAI-specific. Use AdditionalParametersJson for Gemini generationConfig."));
				return false;
			}
			if (!Request.User.IsEmpty())
			{
				OutError = UnrealAIProviderAdapters::MakeError(
					TEXT("The Gemini generateContent API has no core User field mapping. Use AdditionalParametersJson when needed."));
				return false;
			}

			const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> SystemParts;
			TArray<TSharedPtr<FJsonValue>> Contents;
			for (const FUnrealAIChatMessage& Message : Request.Messages)
			{
				if (Message.Role == EUnrealAIMessageRole::Tool)
				{
					OutError = UnrealAIProviderAdapters::MakeError(
						TEXT("Tool-role messages are not normalized for Gemini yet. Use provider-native raw JSON in a future tool adapter."));
					return false;
				}
				if (!Message.Name.IsEmpty() || !Message.ToolCallId.IsEmpty())
				{
					OutError = UnrealAIProviderAdapters::MakeError(
						TEXT("Message Name and ToolCallId are not supported by the core Gemini chat adapter."));
					return false;
				}

				TArray<TSharedPtr<FJsonValue>> Parts;
				if (!Message.ContentJson.TrimStartAndEnd().IsEmpty())
				{
					TSharedPtr<FJsonValue> ContentValue;
					FString ParseError;
					if (!ParseJsonValue(Message.ContentJson, ContentValue, ParseError))
					{
						OutError = UnrealAIProviderAdapters::MakeError(
							FString::Printf(TEXT("Invalid Gemini ContentJson: %s"), *ParseError));
						return false;
					}
					if (ContentValue->Type == EJson::Array)
					{
						Parts = ContentValue->AsArray();
					}
					else if (ContentValue->Type == EJson::Object)
					{
						Parts.Add(ContentValue);
					}
					else
					{
						OutError = UnrealAIProviderAdapters::MakeError(
							TEXT("Gemini ContentJson must contain a native Part object or an array of Part objects."));
						return false;
					}
				}
				else
				{
					const TSharedRef<FJsonObject> TextPart = MakeShared<FJsonObject>();
					TextPart->SetStringField(TEXT("text"), Message.Content);
					Parts.Add(MakeShared<FJsonValueObject>(TextPart));
				}

				if (Message.Role == EUnrealAIMessageRole::System || Message.Role == EUnrealAIMessageRole::Developer)
				{
					if (!Message.AdditionalFieldsJson.TrimStartAndEnd().IsEmpty())
					{
						OutError = UnrealAIProviderAdapters::MakeError(
							TEXT("AdditionalFieldsJson is not supported on Gemini system or developer messages."));
						return false;
					}
					SystemParts.Append(Parts);
					continue;
				}

				const TSharedRef<FJsonObject> ContentObject = MakeShared<FJsonObject>();
				ContentObject->SetStringField(
					TEXT("role"),
					Message.Role == EUnrealAIMessageRole::Assistant ? TEXT("model") : TEXT("user"));
				ContentObject->SetArrayField(TEXT("parts"), Parts);
				if (!MergeAdditionalFields(Message.AdditionalFieldsJson, ContentObject, TEXT("Gemini content"), OutError))
				{
					return false;
				}
				Contents.Add(MakeShared<FJsonValueObject>(ContentObject));
			}

			if (Contents.Num() == 0)
			{
				OutError = UnrealAIProviderAdapters::MakeError(TEXT("Gemini requires at least one user or model message."));
				return false;
			}
			Payload->SetArrayField(TEXT("contents"), Contents);
			if (SystemParts.Num() > 0)
			{
				const TSharedRef<FJsonObject> SystemInstruction = MakeShared<FJsonObject>();
				SystemInstruction->SetArrayField(TEXT("parts"), SystemParts);
				Payload->SetObjectField(TEXT("systemInstruction"), SystemInstruction);
			}

			const TSharedRef<FJsonObject> GenerationConfig = MakeShared<FJsonObject>();
			bool bHasGenerationConfig = false;
			if (Request.bUseTemperature)
			{
				GenerationConfig->SetNumberField(TEXT("temperature"), Request.Temperature);
				bHasGenerationConfig = true;
			}
			if (Request.bUseTopP)
			{
				GenerationConfig->SetNumberField(TEXT("topP"), Request.TopP);
				bHasGenerationConfig = true;
			}
			if (Request.bUseMaxCompletionTokens || Request.bUseLegacyMaxTokens)
			{
				GenerationConfig->SetNumberField(
					TEXT("maxOutputTokens"),
					Request.bUseMaxCompletionTokens ? Request.MaxCompletionTokens : Request.MaxTokens);
				bHasGenerationConfig = true;
			}
			if (Request.StopSequences.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> StopValues;
				for (const FString& StopSequence : Request.StopSequences)
				{
					StopValues.Add(MakeShared<FJsonValueString>(StopSequence));
				}
				GenerationConfig->SetArrayField(TEXT("stopSequences"), StopValues);
				bHasGenerationConfig = true;
			}
			if (bHasGenerationConfig)
			{
				Payload->SetObjectField(TEXT("generationConfig"), GenerationConfig);
			}
			if (!MergeAdditionalParameters(Request.AdditionalParametersJson, Payload, OutError))
			{
				return false;
			}

			FString PathModel = Model;
			PathModel.RemoveFromStart(TEXT("models/"));
			OutRequest = FUnrealAIHttpRequestData();
			OutRequest.Url = BuildEndpointUrl(
				ProviderConfig.BaseUrl,
				FString::Printf(TEXT("models/%s:generateContent"), *FGenericPlatformHttp::UrlEncode(PathModel)));
			OutRequest.ResolvedModel = Model;
			AddCommonHeaders(OutRequest);
			if (!ApiKey.IsEmpty())
			{
				OutRequest.Headers.Add(TEXT("x-goog-api-key"), ApiKey);
			}
			AddAdditionalHeaders(ProviderConfig, OutRequest);

			if (!SerializeJsonObject(Payload, OutRequest.Body))
			{
				OutError = UnrealAIProviderAdapters::MakeError(TEXT("Failed to serialize Gemini generateContent request."));
				return false;
			}
			return true;
		}

		virtual void ParseResponse(
			const FString& ResolvedModel,
			int32 HttpStatus,
			const FString& RawJson,
			FUnrealAIChatResponse& OutResponse,
			FUnrealAIError& OutError) const override
		{
			OutResponse = FUnrealAIChatResponse();
			OutResponse.RawJson = RawJson;
			OutResponse.Object = TEXT("generateContent.response");
			OutError = FUnrealAIError();

			TSharedPtr<FJsonObject> RootObject;
			if (!ParseSuccessfulObject(HttpStatus, RawJson, RootObject, OutError))
			{
				return;
			}

			RootObject->TryGetStringField(TEXT("responseId"), OutResponse.Id);
			if (!RootObject->TryGetStringField(TEXT("modelVersion"), OutResponse.Model))
			{
				OutResponse.Model = ResolvedModel;
			}

			const TSharedPtr<FJsonObject>* UsageObject = nullptr;
			if (RootObject->TryGetObjectField(TEXT("usageMetadata"), UsageObject) && UsageObject && UsageObject->IsValid())
			{
				(*UsageObject)->TryGetNumberField(TEXT("promptTokenCount"), OutResponse.Usage.PromptTokens);
				(*UsageObject)->TryGetNumberField(TEXT("candidatesTokenCount"), OutResponse.Usage.CompletionTokens);
				(*UsageObject)->TryGetNumberField(TEXT("totalTokenCount"), OutResponse.Usage.TotalTokens);
			}

			const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
			if (RootObject->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates)
			{
				for (const TSharedPtr<FJsonValue>& CandidateValue : *Candidates)
				{
					if (!CandidateValue.IsValid() || CandidateValue->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> Candidate = CandidateValue->AsObject();
					FUnrealAIChatChoice Choice;
					Candidate->TryGetNumberField(TEXT("index"), Choice.Index);
					Candidate->TryGetStringField(TEXT("finishReason"), Choice.FinishReason);

					const TSharedPtr<FJsonObject>* ContentObject = nullptr;
					if (Candidate->TryGetObjectField(TEXT("content"), ContentObject) && ContentObject && ContentObject->IsValid())
					{
						(*ContentObject)->TryGetStringField(TEXT("role"), Choice.Role);
						const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
						if ((*ContentObject)->TryGetArrayField(TEXT("parts"), Parts) && Parts)
						{
							for (const TSharedPtr<FJsonValue>& PartValue : *Parts)
							{
								if (!PartValue.IsValid() || PartValue->Type != EJson::Object)
								{
									continue;
								}
								FString Text;
								if (PartValue->AsObject()->TryGetStringField(TEXT("text"), Text))
								{
									Choice.Content += Text;
								}
							}
						}
						Choice.RawMessageJson = JsonObjectToString(*ContentObject);
					}
					OutResponse.Choices.Add(Choice);
				}
			}
		}
	};
}

namespace UnrealAIProviderAdapters
{
	const IUnrealAIProviderAdapter& Get(EUnrealAIProviderApi ProviderApi)
	{
		static UnrealAIProviderAdapterPrivate::FOpenAIProviderAdapter OpenAIAdapter;
		static UnrealAIProviderAdapterPrivate::FAnthropicProviderAdapter AnthropicAdapter;
		static UnrealAIProviderAdapterPrivate::FGeminiProviderAdapter GeminiAdapter;

		switch (ProviderApi)
		{
		case EUnrealAIProviderApi::AnthropicMessages:
			return AnthropicAdapter;
		case EUnrealAIProviderApi::GeminiGenerateContent:
			return GeminiAdapter;
		case EUnrealAIProviderApi::OpenAICompatibleChatCompletions:
		default:
			return OpenAIAdapter;
		}
	}

	FUnrealAIError MakeError(const FString& Message, int32 HttpStatus, const FString& RawJson)
	{
		FUnrealAIError Error;
		Error.bIsError = true;
		Error.HttpStatus = HttpStatus;
		Error.Message = Message;
		Error.RawJson = RawJson;
		return Error;
	}
}
