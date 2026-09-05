# Direct C++ Streaming Client

Use this reference for direct `UUnrealAIClient::StreamChatCompletion` integrations. Read `client-setup.md` only when the task also changes module or provider configuration. Use `chat-component.md` instead for `UUnrealAIChatComponent`.

## Native streaming example

This complete actor is mirrored by `Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIStreamingExample.h` and its matching private `.cpp`. The sample module compiles the exact source shown here. When adapting it, replace `UNREALAISAMPLE_API` with the consuming module's API macro and retain the `UnrealAI` module dependency described above.

<!-- BEGIN COMPILED STREAMING HEADER -->
```cpp
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UnrealAITypes.h"
#include "UnrealAIStreamingExample.generated.h"

class UUnrealAIClient;
struct FUnrealAIStreamingExampleTestAccess;

UCLASS()
class UNREALAISAMPLE_API AUnrealAIStreamingExample : public AActor
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void StartStreaming();

	UFUNCTION(BlueprintCallable, Category = "UnrealAI Example")
	void CancelStreaming();

	UPROPERTY(EditAnywhere, Category = "UnrealAI Example")
	FName ProviderName = TEXT("OpenAI");

	UPROPERTY(EditAnywhere, Category = "UnrealAI Example", meta = (MultiLine = true))
	FString Prompt = TEXT("Describe this level in one sentence.");

	UPROPERTY(BlueprintReadOnly, Category = "UnrealAI Example")
	FString StreamingText;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	friend struct FUnrealAIStreamingExampleTestAccess;

	UPROPERTY()
	TObjectPtr<UUnrealAIClient> UnrealAIClient;

	FUnrealAIRequestHandle ActiveStream;

	void HandleStreamEvent(const FUnrealAIChatStreamEvent& Event);
	void HandleStreamRetry(const FUnrealAIRetryEvent& RetryEvent);
	void HandleStreamTerminal(const FUnrealAIChatStreamResult& Result);
};
```
<!-- END COMPILED STREAMING HEADER -->

<!-- BEGIN COMPILED STREAMING SOURCE -->
```cpp
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
```
<!-- END COMPILED STREAMING SOURCE -->

Call `UnrealAIClient->CancelRequest(ActiveStream)` on the game thread to cancel. A successful call synchronously emits the one terminal `Cancelled` result and preserves the partial normalized response. A false return means the handle was invalid, belonged to a stream that already ended, or was not active on this client.

Event callbacks arrive in provider order on the game thread. Handle `TextDelta` for incremental text, `ChoiceFinished` for finish reasons, `Usage` for normalized token counts, and `ProviderEvent` only when provider-native non-text JSON is required. The terminal callback fires exactly once with `Completed`, `Failed`, or `Cancelled`; failed and cancelled results may contain a partial response.

## Streaming retry and terminal contract

Pass `FUnrealAIRetryNativeDelegate` to observe retries before the delay. A stream is eligible for retry only before the first complete SSE data event. Once any event is accepted, an interruption is terminal and retains partial output instead of replaying the prompt. `CancelRequest` remains valid during backoff.

Callbacks arrive in order on the game thread. Treat the terminal callback as exactly once, preserve partial responses on `Failed` and `Cancelled`, and do not expect aggregate `Response.RawJson`; no single provider object represents the whole stream.

## Verification

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` on a matching native host. The runner compiles the exact marked actor above and executes `UnrealAISample.SkillContracts.Cpp.StreamingBehavior`. Keep both marked blocks synchronized with the sample source; portable skill lint rejects drift before the native build.
