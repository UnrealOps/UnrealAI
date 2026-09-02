#include "UnrealAIBlueprintLibrary.h"

#include "Dom/JsonObject.h"
#include "UnrealAISettings.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FUnrealAIChatMessage UUnrealAIBlueprintLibrary::MakeChatMessage(EUnrealAIMessageRole Role, const FString& Content)
{
	FUnrealAIChatMessage Message;
	Message.Role = Role;
	Message.Content = Content;
	return Message;
}

FUnrealAIChatRequest UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(const FString& Prompt, const FString& Model)
{
	FUnrealAIChatRequest Request;
	Request.Model = Model;

	FUnrealAIChatMessage UserMessage;
	UserMessage.Role = EUnrealAIMessageRole::User;
	UserMessage.Content = Prompt;
	Request.Messages.Add(UserMessage);

	return Request;
}

FString UUnrealAIBlueprintLibrary::MakeJsonObjectResponseFormat()
{
	return TEXT("{\"type\":\"json_object\"}");
}

FString UUnrealAIBlueprintLibrary::MakeStrictJsonSchemaResponseFormat(const FString& SchemaName, const FString& SchemaJson, bool bStrict)
{
	TSharedPtr<FJsonObject> SchemaObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SchemaJson);
	if (!FJsonSerializer::Deserialize(Reader, SchemaObject) || !SchemaObject.IsValid())
	{
		return FString();
	}

	const TSharedRef<FJsonObject> JsonSchemaObject = MakeShared<FJsonObject>();
	JsonSchemaObject->SetStringField(TEXT("name"), SchemaName.IsEmpty() ? TEXT("response_schema") : SchemaName);
	JsonSchemaObject->SetBoolField(TEXT("strict"), bStrict);
	JsonSchemaObject->SetObjectField(TEXT("schema"), SchemaObject);

	const TSharedRef<FJsonObject> ResponseFormatObject = MakeShared<FJsonObject>();
	ResponseFormatObject->SetStringField(TEXT("type"), TEXT("json_schema"));
	ResponseFormatObject->SetObjectField(TEXT("json_schema"), JsonSchemaObject);

	FString Output;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(ResponseFormatObject, Writer);
	return Output;
}

FString UUnrealAIBlueprintLibrary::GetFirstChoiceContent(const FUnrealAIChatResponse& Response, bool& bHasContent)
{
	bHasContent = Response.Choices.Num() > 0 && !Response.Choices[0].Content.IsEmpty();
	return bHasContent ? Response.Choices[0].Content : FString();
}

bool UUnrealAIBlueprintLibrary::ResolveProviderConfig(FName ProviderName, FUnrealAIProviderConfig& ProviderConfig, FUnrealAIError& Error)
{
	const UUnrealAISettings* Settings = GetDefault<UUnrealAISettings>();
	if (!Settings)
	{
		Error.bIsError = true;
		Error.Message = TEXT("UnrealAI settings are unavailable.");
		return false;
	}

	if (!Settings->TryGetProviderConfig(ProviderName, ProviderConfig))
	{
		const FString ResolvedName = ProviderName.IsNone() ? Settings->DefaultProviderName.ToString() : ProviderName.ToString();
		Error.bIsError = true;
		Error.Message = FString::Printf(TEXT("Provider profile '%s' was not found."), *ResolvedName);
		return false;
	}

	Error = FUnrealAIError();
	return true;
}

bool UUnrealAIBlueprintLibrary::ReloadProjectEnvFile(int32& VariablesLoaded, FString& StatusMessage)
{
	return UUnrealAISettings::LoadProjectEnvFile(true, VariablesLoaded, StatusMessage);
}
