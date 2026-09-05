#include "UnrealAISampleActor.h"

#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIClient.h"
#include "UnrealAIProviders.h"

DEFINE_LOG_CATEGORY_STATIC(LogUnrealAISample, Log, All);

AUnrealAISampleActor::AUnrealAISampleActor()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AUnrealAISampleActor::RunCppCompletionSample()
{
	bCppCompletionFinished = false;
	bCppCompletionSucceeded = false;
	CppCompletionText.Reset();
	LastStatus = TEXT("Starting C++ completion...");

	if (!EnsureXAIClient())
	{
		bCppCompletionFinished = true;
		return;
	}

	const FUnrealAIChatRequest Request = UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(Prompt);
	ActiveCompletion = UnrealAIClient->CreateChatCompletion(
		Request,
		FUnrealAIChatCompletionNativeDelegate::CreateUObject(this, &ThisClass::HandleCompletion),
		FUnrealAIRetryNativeDelegate::CreateUObject(this, &ThisClass::HandleRetry));

	if (!ActiveCompletion.IsValid())
	{
		bCppCompletionFinished = true;
		LastStatus = TEXT("UnrealAI did not create a completion request.");
		UE_LOG(LogUnrealAISample, Error, TEXT("%s"), *LastStatus);
	}
}

void AUnrealAISampleActor::RunCppStreamingSample()
{
	bCppStreamFinished = false;
	bCppStreamSucceeded = false;
	CppStreamTextEventCount = 0;
	CppStreamTerminalCount = 0;
	CppStreamingText.Reset();
	LastStatus = TEXT("Starting C++ stream...");

	if (!EnsureXAIClient())
	{
		bCppStreamFinished = true;
		return;
	}

	const FUnrealAIChatRequest Request = UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(Prompt);
	ActiveStream = UnrealAIClient->StreamChatCompletion(
		Request,
		FUnrealAIChatStreamEventNativeDelegate::CreateUObject(this, &ThisClass::HandleStreamEvent),
		FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(this, &ThisClass::HandleStreamTerminal),
		FUnrealAIRetryNativeDelegate::CreateUObject(this, &ThisClass::HandleRetry));

	if (!ActiveStream.IsValid())
	{
		bCppStreamFinished = true;
		LastStatus = TEXT("UnrealAI did not create a streaming request.");
		UE_LOG(LogUnrealAISample, Error, TEXT("%s"), *LastStatus);
	}
}

void AUnrealAISampleActor::CancelCppRequests()
{
	if (!UnrealAIClient)
	{
		return;
	}

	if (ActiveCompletion.IsValid())
	{
		UnrealAIClient->CancelRequest(ActiveCompletion);
	}
	if (ActiveStream.IsValid())
	{
		UnrealAIClient->CancelRequest(ActiveStream);
	}
}

void AUnrealAISampleActor::RecordBlueprintSuccess(const FString& Content, bool bHasContent)
{
	bBlueprintRequestFinished = true;
	bBlueprintRequestSucceeded = bHasContent;
	BlueprintResponseText = Content;
	LastStatus = bHasContent ? TEXT("Blueprint completion succeeded.") : TEXT("Blueprint completion returned no content.");
	UE_LOG(LogUnrealAISample, Log, TEXT("%s"), *LastStatus);
}

void AUnrealAISampleActor::RecordBlueprintFailure(const FUnrealAIError& Error)
{
	bBlueprintRequestFinished = true;
	bBlueprintRequestSucceeded = false;
	LastStatus = Error.Message.IsEmpty() ? TEXT("Blueprint completion failed.") : Error.Message;
	UE_LOG(LogUnrealAISample, Error, TEXT("%s"), *LastStatus);
}

void AUnrealAISampleActor::RecordBlueprintCancellation()
{
	bBlueprintRequestFinished = true;
	bBlueprintRequestSucceeded = false;
	LastStatus = TEXT("Blueprint completion was cancelled.");
	UE_LOG(LogUnrealAISample, Display, TEXT("%s"), *LastStatus);
}

void AUnrealAISampleActor::RecordBlueprintRetry()
{
	LastStatus = TEXT("Blueprint completion is retrying.");
	UE_LOG(LogUnrealAISample, Display, TEXT("%s"), *LastStatus);
}

void AUnrealAISampleActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CancelCppRequests();
	Super::EndPlay(EndPlayReason);
}

bool AUnrealAISampleActor::EnsureXAIClient()
{
	if (UnrealAIClient)
	{
		return true;
	}

	FUnrealAIError ConfigurationError;
	UnrealAIClient = UUnrealAIProviders::XAI(this, ConfigurationError);
	if (!UnrealAIClient)
	{
		LastStatus = ConfigurationError.Message;
		UE_LOG(LogUnrealAISample, Error, TEXT("UnrealAI configuration failed: %s"), *LastStatus);
		return false;
	}

	return true;
}

void AUnrealAISampleActor::HandleCompletion(const FUnrealAIChatResponse& Response, const FUnrealAIError& Error)
{
	ActiveCompletion = FUnrealAIRequestHandle();
	bCppCompletionFinished = true;
	if (Error.bIsError)
	{
		bCppCompletionSucceeded = false;
		LastStatus = Error.Message;
		UE_LOG(LogUnrealAISample, Error, TEXT("C++ completion failed: %s"), *Error.Message);
		return;
	}

	bool bHasContent = false;
	CppCompletionText = UUnrealAIBlueprintLibrary::GetFirstChoiceContent(Response, bHasContent);
	bCppCompletionSucceeded = bHasContent;
	LastStatus = bHasContent ? TEXT("C++ completion succeeded.") : TEXT("C++ completion returned no content.");
	UE_LOG(LogUnrealAISample, Log, TEXT("%s"), *LastStatus);
}

void AUnrealAISampleActor::HandleRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	LastStatus = FString::Printf(
		TEXT("C++ retry %d/%d in %.2f seconds."),
		RetryEvent.RetryNumber,
		RetryEvent.MaxRetries,
		RetryEvent.DelaySeconds);
	UE_LOG(LogUnrealAISample, Display, TEXT("%s"), *LastStatus);
}

void AUnrealAISampleActor::HandleStreamEvent(const FUnrealAIChatStreamEvent& Event)
{
	if (Event.Type == EUnrealAIChatStreamEventType::TextDelta)
	{
		++CppStreamTextEventCount;
		CppStreamingText += Event.TextDelta;
	}
}

void AUnrealAISampleActor::HandleStreamTerminal(const FUnrealAIChatStreamResult& Result)
{
	ActiveStream = FUnrealAIRequestHandle();
	++CppStreamTerminalCount;
	bCppStreamFinished = true;
	bCppStreamSucceeded = Result.Status == EUnrealAIChatStreamStatus::Completed;
	if (Result.Status == EUnrealAIChatStreamStatus::Completed)
	{
		LastStatus = TEXT("C++ stream completed.");
		UE_LOG(LogUnrealAISample, Log, TEXT("%s"), *LastStatus);
		return;
	}

	if (Result.Status == EUnrealAIChatStreamStatus::Cancelled)
	{
		LastStatus = TEXT("C++ stream was cancelled.");
		UE_LOG(LogUnrealAISample, Display, TEXT("%s"), *LastStatus);
		return;
	}

	LastStatus = Result.Error.Message;
	UE_LOG(LogUnrealAISample, Error, TEXT("C++ stream failed: %s"), *LastStatus);
}
