#include "UnrealAIClient.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "UnrealAI.h"
#include "UnrealAISettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UnrealAIClientPrivate
{
	FString RoleToString(EUnrealAIMessageRole Role)
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

	FUnrealAIError MakeUnrealAIError(const FString& Message, int32 HttpStatus = 0, const FString& RawJson = TEXT(""))
	{
		FUnrealAIError Error;
		Error.bIsError = true;
		Error.HttpStatus = HttpStatus;
		Error.Message = Message;
		Error.RawJson = RawJson;
		return Error;
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

	FUnrealAIError ParseApiError(int32 HttpStatus, const FString& RawJson)
	{
		FUnrealAIError Error = MakeUnrealAIError(TEXT("The provider returned an error."), HttpStatus, RawJson);

		TSharedPtr<FJsonObject> RootObject;
		FString ParseError;
		if (!ParseJsonObject(RawJson, RootObject, ParseError))
		{
			Error.Message = RawJson.IsEmpty() ? FString::Printf(TEXT("HTTP request failed with status %d."), HttpStatus) : RawJson;
			return Error;
		}

		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		if (RootObject->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && ErrorObject->IsValid())
		{
			(*ErrorObject)->TryGetStringField(TEXT("message"), Error.Message);
			(*ErrorObject)->TryGetStringField(TEXT("type"), Error.Type);
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
		}

		if (Error.Message.IsEmpty())
		{
			Error.Message = FString::Printf(TEXT("HTTP request failed with status %d."), HttpStatus);
		}

		return Error;
	}

	FString JsonObjectToString(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return TEXT("");
		}

		FString OutJson;
		SerializeJsonObject(Object.ToSharedRef(), OutJson);
		return OutJson;
	}
}

void UUnrealAIClient::Configure(const FUnrealAIProviderConfig& InProviderConfig)
{
	ProviderConfig = InProviderConfig;
	bConfigured = true;
}

bool UUnrealAIClient::ConfigureFromSettings(FName ProviderName, FUnrealAIError& OutError)
{
	const UUnrealAISettings* Settings = GetDefault<UUnrealAISettings>();
	if (!Settings)
	{
		OutError = UnrealAIClientPrivate::MakeUnrealAIError(TEXT("UnrealAI settings are unavailable."));
		return false;
	}

	FUnrealAIProviderConfig ResolvedProvider;
	if (!Settings->TryGetProviderConfig(ProviderName, ResolvedProvider))
	{
		const FString ResolvedName = ProviderName.IsNone() ? Settings->DefaultProviderName.ToString() : ProviderName.ToString();
		OutError = UnrealAIClientPrivate::MakeUnrealAIError(FString::Printf(TEXT("Provider profile '%s' was not found."), *ResolvedName));
		return false;
	}

	Configure(ResolvedProvider);
	return true;
}

bool UUnrealAIClient::IsConfigured() const
{
	return bConfigured;
}

const FUnrealAIProviderConfig& UUnrealAIClient::GetProviderConfig() const
{
	return ProviderConfig;
}

void UUnrealAIClient::CreateChatCompletion(const FUnrealAIChatRequest& Request, FUnrealAIChatCompletionNativeDelegate CompletionDelegate)
{
	if (!bConfigured)
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(EmptyResponse, UnrealAIClientPrivate::MakeUnrealAIError(TEXT("Client is not configured.")));
		return;
	}

	const FString ApiKey = ResolveApiKey();
	if (ProviderConfig.bRequiresApiKey && ApiKey.IsEmpty())
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(EmptyResponse, UnrealAIClientPrivate::MakeUnrealAIError(
			FString::Printf(TEXT("API key is not configured. Set %s or provide an API key override."), *ProviderConfig.ApiKeyEnvironmentVariable)));
		return;
	}

	FString Payload;
	FUnrealAIError BuildError;
	if (!BuildChatCompletionPayload(Request, Payload, BuildError))
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(EmptyResponse, BuildError);
		return;
	}

	FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(BuildEndpointUrl(TEXT("chat/completions")));
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
	HttpRequest->SetTimeout(FMath::Max(1.0f, ProviderConfig.TimeoutSeconds));

	if (!ApiKey.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}

	if (!ProviderConfig.OrganizationId.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("OpenAI-Organization"), ProviderConfig.OrganizationId);
	}

	if (!ProviderConfig.ProjectId.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("OpenAI-Project"), ProviderConfig.ProjectId);
	}

	for (const TPair<FString, FString>& Header : ProviderConfig.AdditionalHeaders)
	{
		if (!Header.Key.IsEmpty())
		{
			HttpRequest->SetHeader(Header.Key, Header.Value);
		}
	}

	HttpRequest->SetContentAsString(Payload);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UUnrealAIClient::HandleChatCompletionResponse, CompletionDelegate);

	InFlightRequests.Add(HttpRequest);

	if (!HttpRequest->ProcessRequest())
	{
		InFlightRequests.Remove(HttpRequest);

		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(EmptyResponse, UnrealAIClientPrivate::MakeUnrealAIError(TEXT("Failed to start HTTP request.")));
	}
}

FString UUnrealAIClient::ResolveApiKey() const
{
	if (!ProviderConfig.ApiKeyOverride.IsEmpty())
	{
		return ProviderConfig.ApiKeyOverride;
	}

	if (!ProviderConfig.ApiKeyEnvironmentVariable.IsEmpty())
	{
		return FPlatformMisc::GetEnvironmentVariable(*ProviderConfig.ApiKeyEnvironmentVariable);
	}

	return TEXT("");
}

FString UUnrealAIClient::BuildEndpointUrl(const FString& Path) const
{
	FString BaseUrl = ProviderConfig.BaseUrl.TrimStartAndEnd();
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}

	FString NormalizedPath = Path;
	while (NormalizedPath.StartsWith(TEXT("/")))
	{
		NormalizedPath.RightChopInline(1);
	}

	return FString::Printf(TEXT("%s/%s"), *BaseUrl, *NormalizedPath);
}

bool UUnrealAIClient::BuildChatCompletionPayload(const FUnrealAIChatRequest& Request, FString& OutPayload, FUnrealAIError& OutError) const
{
	using namespace UnrealAIClientPrivate;

	if (Request.bStream)
	{
		OutError = MakeUnrealAIError(TEXT("Streaming chat completions are not implemented in this SDK build. Send a non-streaming request or add an SSE handler."));
		return false;
	}

	const FString Model = Request.Model.IsEmpty() ? ProviderConfig.DefaultModel : Request.Model;
	if (Model.IsEmpty())
	{
		OutError = MakeUnrealAIError(TEXT("No model was specified and the provider profile does not define a default model."));
		return false;
	}

	if (Request.Messages.Num() == 0)
	{
		OutError = MakeUnrealAIError(TEXT("Chat completion request must contain at least one message."));
		return false;
	}

	const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("model"), Model);

	TArray<TSharedPtr<FJsonValue>> Messages;
	for (const FUnrealAIChatMessage& Message : Request.Messages)
	{
		const TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
		MessageObject->SetStringField(TEXT("role"), RoleToString(Message.Role));

		if (!Message.ContentJson.TrimStartAndEnd().IsEmpty())
		{
			TSharedPtr<FJsonValue> ContentValue;
			FString ParseError;
			if (!ParseJsonValue(Message.ContentJson, ContentValue, ParseError))
			{
				OutError = MakeUnrealAIError(FString::Printf(TEXT("Invalid ContentJson for message: %s"), *ParseError));
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

		if (!Message.AdditionalFieldsJson.TrimStartAndEnd().IsEmpty())
		{
			TSharedPtr<FJsonObject> AdditionalFields;
			FString ParseError;
			if (!ParseJsonObject(Message.AdditionalFieldsJson, AdditionalFields, ParseError))
			{
				OutError = MakeUnrealAIError(FString::Printf(TEXT("Invalid AdditionalFieldsJson for message: %s"), *ParseError));
				return false;
			}
			MergeJsonObject(MessageObject, AdditionalFields.ToSharedRef());
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
			OutError = MakeUnrealAIError(FString::Printf(TEXT("Invalid ResponseFormatJson: %s"), *ParseError));
			return false;
		}
		Payload->SetObjectField(TEXT("response_format"), ResponseFormat);
	}

	if (!Request.AdditionalParametersJson.TrimStartAndEnd().IsEmpty())
	{
		TSharedPtr<FJsonObject> AdditionalParameters;
		FString ParseError;
		if (!ParseJsonObject(Request.AdditionalParametersJson, AdditionalParameters, ParseError))
		{
			OutError = MakeUnrealAIError(FString::Printf(TEXT("Invalid AdditionalParametersJson: %s"), *ParseError));
			return false;
		}
		MergeJsonObject(Payload, AdditionalParameters.ToSharedRef());
	}

	if (!SerializeJsonObject(Payload, OutPayload))
	{
		OutError = MakeUnrealAIError(TEXT("Failed to serialize chat completion request."));
		return false;
	}

	return true;
}

void UUnrealAIClient::HandleChatCompletionResponse(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bWasSuccessful, FUnrealAIChatCompletionNativeDelegate CompletionDelegate)
{
	InFlightRequests.Remove(HttpRequest);

	FUnrealAIChatResponse ParsedResponse;
	FUnrealAIError Error;

	if (!bWasSuccessful || !HttpResponse.IsValid())
	{
		Error = UnrealAIClientPrivate::MakeUnrealAIError(TEXT("HTTP request failed before a provider response was received."));
		CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
		return;
	}

	const int32 ResponseCode = HttpResponse->GetResponseCode();
	const FString RawBody = HttpResponse->GetContentAsString();
	ParsedResponse.RawJson = RawBody;

	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		Error = UnrealAIClientPrivate::ParseApiError(ResponseCode, RawBody);
		CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	FString ParseError;
	if (!UnrealAIClientPrivate::ParseJsonObject(RawBody, RootObject, ParseError))
	{
		Error = UnrealAIClientPrivate::MakeUnrealAIError(FString::Printf(TEXT("Provider returned invalid JSON: %s"), *ParseError), ResponseCode, RawBody);
		CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
		return;
	}

	const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("error"), ErrorObject))
	{
		Error = UnrealAIClientPrivate::ParseApiError(ResponseCode, RawBody);
		CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
		return;
	}

	RootObject->TryGetStringField(TEXT("id"), ParsedResponse.Id);
	RootObject->TryGetStringField(TEXT("object"), ParsedResponse.Object);
	RootObject->TryGetStringField(TEXT("model"), ParsedResponse.Model);
	RootObject->TryGetNumberField(TEXT("created"), ParsedResponse.CreatedUnixTime);

	const TSharedPtr<FJsonObject>* UsageObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("usage"), UsageObject) && UsageObject && UsageObject->IsValid())
	{
		(*UsageObject)->TryGetNumberField(TEXT("prompt_tokens"), ParsedResponse.Usage.PromptTokens);
		(*UsageObject)->TryGetNumberField(TEXT("completion_tokens"), ParsedResponse.Usage.CompletionTokens);
		(*UsageObject)->TryGetNumberField(TEXT("total_tokens"), ParsedResponse.Usage.TotalTokens);
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
			FUnrealAIChatChoice ParsedChoice;
			ChoiceObject->TryGetNumberField(TEXT("index"), ParsedChoice.Index);
			ChoiceObject->TryGetStringField(TEXT("finish_reason"), ParsedChoice.FinishReason);

			const TSharedPtr<FJsonObject>* MessageObject = nullptr;
			if (ChoiceObject->TryGetObjectField(TEXT("message"), MessageObject) && MessageObject && MessageObject->IsValid())
			{
				(*MessageObject)->TryGetStringField(TEXT("role"), ParsedChoice.Role);
				(*MessageObject)->TryGetStringField(TEXT("content"), ParsedChoice.Content);
				ParsedChoice.RawMessageJson = UnrealAIClientPrivate::JsonObjectToString(*MessageObject);
			}

			ParsedResponse.Choices.Add(ParsedChoice);
		}
	}

	CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
}
