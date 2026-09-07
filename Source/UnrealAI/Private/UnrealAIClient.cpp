// Copyright EngineWorks. All Rights Reserved.

#include "UnrealAIClient.h"
#include "UnrealAIExecutionService.h"
#include "UnrealAIProviderAdapter.h"
#include "UnrealAISettings.h"

TSharedRef<FUnrealAIExecutionService, ESPMode::ThreadSafe> UUnrealAIClient::GetExecution() const
{
	check(IsInGameThread());
	if (!Execution)
	{
		Execution = MakeShared<FUnrealAIExecutionService, ESPMode::ThreadSafe>();
	}
	return Execution.ToSharedRef();
}

void UUnrealAIClient::Configure(const FUnrealAIProviderConfig &InProviderConfig)
{
	GetExecution()->Configure(InProviderConfig);
}

bool UUnrealAIClient::ConfigureFromSettings(FName ProviderName, FUnrealAIError &OutError)
{
	const UUnrealAISettings *Settings = GetDefault<UUnrealAISettings>();
	if (!Settings)
	{
		OutError = UnrealAIProviderAdapters::MakeError(TEXT("UnrealAI settings are unavailable."));
		return false;
	}

	FUnrealAIProviderConfig ResolvedProvider;
	if (!Settings->TryGetProviderConfig(ProviderName, ResolvedProvider))
	{
		const FString ResolvedName =
			ProviderName.IsNone() ? Settings->DefaultProviderName.ToString() : ProviderName.ToString();
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
	return GetExecution()->IsConfigured();
}

const FUnrealAIProviderConfig &UUnrealAIClient::GetProviderConfig() const
{
	return GetExecution()->GetProviderConfig();
}

FUnrealAIResponseCapabilities UUnrealAIClient::GetResponseCapabilities() const
{
	return GetExecution()->GetResponseCapabilities();
}

bool UUnrealAIClient::ValidateResponseRequest(const FUnrealAIResponseRequest &Request, FUnrealAIError &OutError) const
{
	return GetExecution()->ValidateResponseRequest(Request, OutError);
}

FUnrealAIRequestHandle UUnrealAIClient::CreateChatCompletion(const FUnrealAIChatRequest &Request,
															 FUnrealAIChatCompletionNativeDelegate CompletionDelegate,
															 FUnrealAIRetryNativeDelegate RetryDelegate)
{
	return GetExecution()->CreateChatCompletion(Request, MoveTemp(CompletionDelegate), MoveTemp(RetryDelegate));
}

FUnrealAIRequestHandle UUnrealAIClient::StreamChatCompletion(const FUnrealAIChatRequest &Request,
															 FUnrealAIChatStreamEventNativeDelegate EventDelegate,
															 FUnrealAIChatStreamTerminalNativeDelegate TerminalDelegate,
															 FUnrealAIRetryNativeDelegate RetryDelegate)
{
	return GetExecution()->StreamChatCompletion(Request, MoveTemp(EventDelegate), MoveTemp(TerminalDelegate),
												MoveTemp(RetryDelegate));
}

FUnrealAIRequestHandle UUnrealAIClient::CreateResponse(const FUnrealAIResponseRequest &Request,
													   FUnrealAIResponseNativeDelegate CompletionDelegate,
													   FUnrealAIRetryNativeDelegate RetryDelegate)
{
	return GetExecution()->CreateResponse(Request, MoveTemp(CompletionDelegate), MoveTemp(RetryDelegate));
}

FUnrealAIRequestHandle UUnrealAIClient::StreamResponse(const FUnrealAIResponseRequest &Request,
													   FUnrealAIResponseEventNativeDelegate EventDelegate,
													   FUnrealAIResponseNativeDelegate TerminalDelegate,
													   FUnrealAIRetryNativeDelegate RetryDelegate)
{
	return GetExecution()->StreamResponse(Request, MoveTemp(EventDelegate), MoveTemp(TerminalDelegate),
										  MoveTemp(RetryDelegate));
}

bool UUnrealAIClient::CancelRequest(const FUnrealAIRequestHandle &RequestHandle)
{
	return Execution && Execution->CancelRequest(RequestHandle);
}

void UUnrealAIClient::BeginDestroy()
{
	if (Execution)
	{
		Execution->Shutdown();
		Execution.Reset();
	}
	Super::BeginDestroy();
}
