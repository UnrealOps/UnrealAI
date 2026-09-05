#include "UnrealAIStreamingExample.h"

#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAIStreamingExample, Log, All);

void AUnrealAIStreamingExample::StartStreaming()
{
	if (ActiveStream.IsValid())
	{
		UE_LOG(LogUnrealAIStreamingExample, Warning, TEXT("A stream is already active."));
		return;
	}

	if (!UnrealAIClient)
	{
		UnrealAIClient = NewObject<UUnrealAIClient>(this);
	}

	FUnrealAIError ConfigurationError;
	if (!UnrealAIClient->ConfigureFromSettings(ProviderName, ConfigurationError))
	{
		UE_LOG(
			LogUnrealAIStreamingExample,
			Error,
			TEXT("UnrealAI configuration failed: %s"),
			*ConfigurationError.Message);
		return;
	}

	StreamingText.Reset();
	const FUnrealAIChatRequest Request =
		UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(Prompt);

	ActiveStream = UnrealAIClient->StreamChatCompletion(
		Request,
		FUnrealAIChatStreamEventNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleStreamEvent),
		FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleStreamTerminal),
		FUnrealAIRetryNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleStreamRetry));

	if (!ActiveStream.IsValid())
	{
		UE_LOG(LogUnrealAIStreamingExample, Error, TEXT("UnrealAI did not start the stream."));
	}
}

void AUnrealAIStreamingExample::CancelStreaming()
{
	if (UnrealAIClient && ActiveStream.IsValid()
		&& !UnrealAIClient->CancelRequest(ActiveStream))
	{
		ActiveStream = FUnrealAIRequestHandle();
	}
}

void AUnrealAIStreamingExample::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CancelStreaming();
	Super::EndPlay(EndPlayReason);
}

void AUnrealAIStreamingExample::HandleStreamEvent(const FUnrealAIChatStreamEvent& Event)
{
	if (Event.Type == EUnrealAIChatStreamEventType::TextDelta)
	{
		StreamingText += Event.TextDelta;
	}
}

void AUnrealAIStreamingExample::HandleStreamRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	UE_LOG(
		LogUnrealAIStreamingExample,
		Verbose,
		TEXT("UnrealAI retry %d/%d in %.2f seconds."),
		RetryEvent.RetryNumber,
		RetryEvent.MaxRetries,
		RetryEvent.DelaySeconds);
}

void AUnrealAIStreamingExample::HandleStreamTerminal(const FUnrealAIChatStreamResult& Result)
{
	ActiveStream = FUnrealAIRequestHandle();

	bool bHasAggregateText = false;
	const FString AggregateText =
		UUnrealAIBlueprintLibrary::GetFirstChoiceContent(Result.Response, bHasAggregateText);
	if (bHasAggregateText)
	{
		StreamingText = AggregateText;
	}

	switch (Result.Status)
	{
	case EUnrealAIChatStreamStatus::Completed:
		UE_LOG(LogUnrealAIStreamingExample, Log, TEXT("UnrealAI stream completed."));
		break;
	case EUnrealAIChatStreamStatus::Cancelled:
		UE_LOG(LogUnrealAIStreamingExample, Display, TEXT("UnrealAI stream was cancelled."));
		break;
	case EUnrealAIChatStreamStatus::Failed:
	default:
		UE_LOG(
			LogUnrealAIStreamingExample,
			Error,
			TEXT("UnrealAI stream failed: %s"),
			*Result.Error.Message);
		break;
	}
}
