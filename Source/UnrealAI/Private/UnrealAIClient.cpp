#include "UnrealAIClient.h"

#include "HttpModule.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IHttpResponse.h"
#include "UnrealAIProviderAdapter.h"
#include "UnrealAISettings.h"

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
		OutError = UnrealAIProviderAdapters::MakeError(TEXT("UnrealAI settings are unavailable."));
		return false;
	}

	FUnrealAIProviderConfig ResolvedProvider;
	if (!Settings->TryGetProviderConfig(ProviderName, ResolvedProvider))
	{
		const FString ResolvedName = ProviderName.IsNone() ? Settings->DefaultProviderName.ToString() : ProviderName.ToString();
		OutError = UnrealAIProviderAdapters::MakeError(
			FString::Printf(TEXT("Provider profile '%s' was not found."), *ResolvedName));
		return false;
	}

	Configure(ResolvedProvider);
	OutError = FUnrealAIError();
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

void UUnrealAIClient::CreateChatCompletion(
	const FUnrealAIChatRequest& Request,
	FUnrealAIChatCompletionNativeDelegate CompletionDelegate)
{
	if (!bConfigured)
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(
			EmptyResponse,
			UnrealAIProviderAdapters::MakeError(TEXT("Client is not configured.")));
		return;
	}

	if (Request.bStream)
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(
			EmptyResponse,
			UnrealAIProviderAdapters::MakeError(
				TEXT("Streaming chat completions are not implemented in this SDK build. Send a non-streaming request or add an SSE handler.")));
		return;
	}

	const FString ApiKey = ResolveApiKey();
	if (ProviderConfig.bRequiresApiKey && ApiKey.IsEmpty())
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(
			EmptyResponse,
			UnrealAIProviderAdapters::MakeError(
				FString::Printf(
					TEXT("API key is not configured. Set %s or provide an API key override."),
					*ProviderConfig.ApiKeyEnvironmentVariable)));
		return;
	}

	const IUnrealAIProviderAdapter& Adapter = UnrealAIProviderAdapters::Get(ProviderConfig.Api);
	FUnrealAIHttpRequestData RequestData;
	FUnrealAIError BuildError;
	if (!Adapter.BuildRequest(ProviderConfig, Request, ApiKey, RequestData, BuildError))
	{
		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(EmptyResponse, BuildError);
		return;
	}

	FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(RequestData.Url);
	HttpRequest->SetVerb(RequestData.Verb);
	HttpRequest->SetTimeout(FMath::Max(1.0f, ProviderConfig.TimeoutSeconds));
	for (const TPair<FString, FString>& Header : RequestData.Headers)
	{
		HttpRequest->SetHeader(Header.Key, Header.Value);
	}
	HttpRequest->SetContentAsString(RequestData.Body);
	HttpRequest->OnProcessRequestComplete().BindUObject(
		this,
		&UUnrealAIClient::HandleChatCompletionResponse,
		ProviderConfig.Api,
		RequestData.ResolvedModel,
		CompletionDelegate);

	InFlightRequests.Add(HttpRequest);
	if (!HttpRequest->ProcessRequest())
	{
		InFlightRequests.Remove(HttpRequest);

		FUnrealAIChatResponse EmptyResponse;
		CompletionDelegate.ExecuteIfBound(
			EmptyResponse,
			UnrealAIProviderAdapters::MakeError(TEXT("Failed to start HTTP request.")));
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

	return FString();
}

void UUnrealAIClient::HandleChatCompletionResponse(
	FHttpRequestPtr HttpRequest,
	FHttpResponsePtr HttpResponse,
	bool bWasSuccessful,
	EUnrealAIProviderApi ProviderApi,
	FString ResolvedModel,
	FUnrealAIChatCompletionNativeDelegate CompletionDelegate)
{
	InFlightRequests.Remove(HttpRequest);

	FUnrealAIChatResponse ParsedResponse;
	FUnrealAIError Error;
	if (!bWasSuccessful || !HttpResponse.IsValid())
	{
		Error = UnrealAIProviderAdapters::MakeError(
			TEXT("HTTP request failed before a provider response was received."));
		CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
		return;
	}

	const IUnrealAIProviderAdapter& Adapter = UnrealAIProviderAdapters::Get(ProviderApi);
	Adapter.ParseResponse(
		ResolvedModel,
		HttpResponse->GetResponseCode(),
		HttpResponse->GetContentAsString(),
		ParsedResponse,
		Error);
	CompletionDelegate.ExecuteIfBound(ParsedResponse, Error);
}
