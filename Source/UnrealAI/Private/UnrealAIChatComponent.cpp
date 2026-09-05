#include "UnrealAIChatComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tests/UnrealAIChatComponentTestSupport.h"
#endif

UUnrealAIChatComponent::UUnrealAIChatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UUnrealAIChatComponent::SendPrompt(const FString& Prompt)
{
	SendMessages(BuildPromptMessages(Prompt));
}

void UUnrealAIChatComponent::SendMessages(const TArray<FUnrealAIChatMessage>& Messages)
{
	FUnrealAIError ConfigError;
	if (!EnsureClient(ConfigError))
	{
		FUnrealAIChatResponse EmptyResponse;
		OnChatFailed.Broadcast(EmptyResponse, ConfigError);
		return;
	}

	const FUnrealAIChatRequest Request = BuildRequest(Messages);

	const FGuid CompletionId = FGuid::NewGuid();
	const FUnrealAIRequestHandle RequestHandle = Client->CreateChatCompletion(
		Request,
		FUnrealAIChatCompletionNativeDelegate::CreateWeakLambda(
			this,
			[this, CompletionId](const FUnrealAIChatResponse& Response, const FUnrealAIError& Error)
			{
				HandleCompletion(CompletionId, Response, Error);
			}),
		FUnrealAIRetryNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleRetry));
	if (RequestHandle.IsValid())
	{
		ActiveCompletionHandles.Add(CompletionId, RequestHandle);
	}
}

int32 UUnrealAIChatComponent::CancelActiveCompletions()
{
	if (!Client || ActiveCompletionHandles.IsEmpty())
	{
		return 0;
	}

	TArray<FUnrealAIRequestHandle> Handles;
	ActiveCompletionHandles.GenerateValueArray(Handles);
	int32 CancelledCount = 0;
	for (const FUnrealAIRequestHandle& Handle : Handles)
	{
		if (Client->CancelRequest(Handle))
		{
			++CancelledCount;
		}
	}
	return CancelledCount;
}

void UUnrealAIChatComponent::SendPromptStream(const FString& Prompt)
{
	SendMessagesStream(BuildPromptMessages(Prompt));
}

void UUnrealAIChatComponent::SendMessagesStream(const TArray<FUnrealAIChatMessage>& Messages)
{
	if (ActiveStreamHandle.IsValid())
	{
		FUnrealAIChatResponse EmptyResponse;
		FUnrealAIError Error;
		Error.bIsError = true;
		Error.Type = TEXT("stream_error");
		Error.Code = TEXT("stream_already_active");
		Error.Message = TEXT("This UnrealAIChatComponent already has an active stream.");
		OnChatStreamFailed.Broadcast(EmptyResponse, Error);
		return;
	}

	FUnrealAIError ConfigError;
	if (!EnsureClient(ConfigError))
	{
		FUnrealAIChatResponse EmptyResponse;
		OnChatStreamFailed.Broadcast(EmptyResponse, ConfigError);
		return;
	}

	const FUnrealAIChatRequest Request = BuildRequest(Messages);
	ActiveStreamHandle = Client->StreamChatCompletion(
		Request,
		FUnrealAIChatStreamEventNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleStreamEvent),
		FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleStreamTerminal),
		FUnrealAIRetryNativeDelegate::CreateUObject(this, &UUnrealAIChatComponent::HandleStreamRetry));
}

bool UUnrealAIChatComponent::CancelActiveStream()
{
	return Client && ActiveStreamHandle.IsValid() && Client->CancelRequest(ActiveStreamHandle);
}

TArray<FUnrealAIChatMessage> UUnrealAIChatComponent::BuildPromptMessages(const FString& Prompt) const
{
	TArray<FUnrealAIChatMessage> Messages;
	if (!SystemPrompt.IsEmpty())
	{
		FUnrealAIChatMessage SystemMessage;
		SystemMessage.Role = EUnrealAIMessageRole::System;
		SystemMessage.Content = SystemPrompt;
		Messages.Add(SystemMessage);
	}

	FUnrealAIChatMessage UserMessage;
	UserMessage.Role = EUnrealAIMessageRole::User;
	UserMessage.Content = Prompt;
	Messages.Add(UserMessage);
	return Messages;
}

FUnrealAIChatRequest UUnrealAIChatComponent::BuildRequest(
	const TArray<FUnrealAIChatMessage>& Messages) const
{
	FUnrealAIChatRequest Request;
	Request.Model = Model;
	Request.Messages = Messages;
	Request.bUseTemperature = bUseTemperature;
	Request.Temperature = Temperature;
	Request.RetryOptions = RetryOptions;
	return Request;
}

bool UUnrealAIChatComponent::EnsureClient(FUnrealAIError& OutError)
{
	if (!Client)
	{
		Client = NewObject<UUnrealAIClient>(this);
	}

	return Client->ConfigureFromSettings(ProviderName, OutError);
}

void UUnrealAIChatComponent::HandleCompletion(
	FGuid CompletionId,
	const FUnrealAIChatResponse& Response,
	const FUnrealAIError& Error)
{
	ActiveCompletionHandles.Remove(CompletionId);
	if (bEndingPlay)
	{
		return;
	}

	if (Error.Code == TEXT("request_cancelled"))
	{
		OnChatCancelled.Broadcast(Response, Error);
	}
	else if (Error.bIsError)
	{
		OnChatFailed.Broadcast(Response, Error);
	}
	else
	{
		OnChatCompleted.Broadcast(Response, Error);
	}
}

void UUnrealAIChatComponent::HandleRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	if (!bEndingPlay)
	{
		OnChatRetrying.Broadcast(RetryEvent);
	}
}

void UUnrealAIChatComponent::HandleStreamEvent(const FUnrealAIChatStreamEvent& Event)
{
	if (!bEndingPlay)
	{
		OnChatStreamEvent.Broadcast(Event);
	}
}

void UUnrealAIChatComponent::HandleStreamRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	if (!bEndingPlay)
	{
		OnChatStreamRetrying.Broadcast(RetryEvent);
	}
}

void UUnrealAIChatComponent::HandleStreamTerminal(const FUnrealAIChatStreamResult& Result)
{
	ActiveStreamHandle = FUnrealAIRequestHandle();
	if (bEndingPlay)
	{
		return;
	}

	switch (Result.Status)
	{
	case EUnrealAIChatStreamStatus::Completed:
		OnChatStreamCompleted.Broadcast(Result.Response, Result.Error);
		break;
	case EUnrealAIChatStreamStatus::Cancelled:
		OnChatStreamCancelled.Broadcast(Result.Response);
		break;
	case EUnrealAIChatStreamStatus::Failed:
	default:
		OnChatStreamFailed.Broadcast(Result.Response, Result.Error);
		break;
	}
}

void UUnrealAIChatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	CancelActiveCompletions();
	ActiveCompletionHandles.Reset();
	if (Client && ActiveStreamHandle.IsValid())
	{
		Client->CancelRequest(ActiveStreamHandle);
		ActiveStreamHandle = FUnrealAIRequestHandle();
	}
	Super::EndPlay(EndPlayReason);
}

#if WITH_DEV_AUTOMATION_TESTS

void FUnrealAIChatComponentTestAccess::RunRequestConstructionTests(FAutomationTestBase& Test)
{
	UUnrealAIChatComponent* Component = NewObject<UUnrealAIChatComponent>();
	Test.TestNotNull(TEXT("The request-construction test component is created"), Component);
	if (!Component)
	{
		return;
	}

	Component->SystemPrompt = TEXT("System contract");
	Component->Model = TEXT("model-contract");
	Component->bUseTemperature = true;
	Component->Temperature = 0.25f;
	Component->RetryOptions.Mode = EUnrealAIRetryMode::OverrideMaxRetries;
	Component->RetryOptions.MaxRetries = 1;

	const TArray<FUnrealAIChatMessage> PromptMessages = Component->BuildPromptMessages(TEXT("User contract"));
	Test.TestEqual(TEXT("Prompt helpers create two messages when a system prompt is set"), PromptMessages.Num(), 2);
	if (PromptMessages.Num() == 2)
	{
		Test.TestEqual(TEXT("Prompt helpers put the system message first"), PromptMessages[0].Role, EUnrealAIMessageRole::System);
		Test.TestEqual(TEXT("Prompt helpers preserve the system prompt"), PromptMessages[0].Content, Component->SystemPrompt);
		Test.TestEqual(TEXT("Prompt helpers put the user message second"), PromptMessages[1].Role, EUnrealAIMessageRole::User);
		Test.TestEqual(TEXT("Prompt helpers preserve the user prompt"), PromptMessages[1].Content, FString(TEXT("User contract")));
	}

	TArray<FUnrealAIChatMessage> SuppliedHistory;
	FUnrealAIChatMessage SuppliedMessage;
	SuppliedMessage.Role = EUnrealAIMessageRole::Assistant;
	SuppliedMessage.Content = TEXT("Caller-owned history");
	SuppliedHistory.Add(SuppliedMessage);
	const FUnrealAIChatRequest Request = Component->BuildRequest(SuppliedHistory);
	Test.TestEqual(TEXT("Messages requests preserve the caller-owned history exactly"), Request.Messages.Num(), 1);
	if (Request.Messages.Num() == 1)
	{
		Test.TestEqual(TEXT("Messages requests do not inject the component system prompt"), Request.Messages[0].Role, EUnrealAIMessageRole::Assistant);
		Test.TestEqual(TEXT("Messages requests preserve content"), Request.Messages[0].Content, SuppliedMessage.Content);
	}
	Test.TestEqual(TEXT("Request construction preserves the model"), Request.Model, Component->Model);
	Test.TestTrue(TEXT("Request construction preserves the temperature toggle"), Request.bUseTemperature);
	Test.TestEqual(TEXT("Request construction preserves temperature"), Request.Temperature, Component->Temperature);
	Test.TestEqual(TEXT("Request construction preserves retry mode"), Request.RetryOptions.Mode, Component->RetryOptions.Mode);
	Test.TestEqual(TEXT("Request construction preserves retry count"), Request.RetryOptions.MaxRetries, 1);
}

#endif
