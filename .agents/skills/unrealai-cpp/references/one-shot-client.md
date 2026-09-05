# Direct C++ One-Shot Client

Use this reference for `UUnrealAIClient::CreateChatCompletion`. Read `client-setup.md` only when the task also changes module, provider, or advanced request configuration. Use `chat-component.md` for component-owned calls and `streaming-client.md` for SSE.

## Ownership

Retain both the client and request handle for cancellation:

```cpp
UPROPERTY()
TObjectPtr<UUnrealAIClient> UnrealAIClient;

FUnrealAIRequestHandle ActiveCompletion;
```

The client must have an appropriate outer. Do not create an unreferenced temporary client inside the submission function.

## Submit and complete

```cpp
void AMyAIActor::AskUnrealAI()
{
	FUnrealAIError ConfigurationError;
	UnrealAIClient = UUnrealAIProviders::OpenAI(this, ConfigurationError);
	if (!UnrealAIClient)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealAI configuration failed: %s"), *ConfigurationError.Message);
		return;
	}

	const FUnrealAIChatRequest Request =
		UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(
			TEXT("Describe this level in one sentence."));

	ActiveCompletion = UnrealAIClient->CreateChatCompletion(
		Request,
		FUnrealAIChatCompletionNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleChatCompletion),
		FUnrealAIRetryNativeDelegate::CreateUObject(
			this,
			&ThisClass::HandleRetry));
}

void AMyAIActor::HandleChatCompletion(
	const FUnrealAIChatResponse& Response,
	const FUnrealAIError& Error)
{
	ActiveCompletion = FUnrealAIRequestHandle();
	if (Error.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealAI request failed: %s"), *Error.Message);
		return;
	}

	bool bHasContent = false;
	const FString Content =
		UUnrealAIBlueprintLibrary::GetFirstChoiceContent(Response, bHasContent);
	if (bHasContent)
	{
		UE_LOG(LogTemp, Log, TEXT("UnrealAI: %s"), *Content);
	}
}

void AMyAIActor::HandleRetry(const FUnrealAIRetryEvent& RetryEvent)
{
	UE_LOG(
		LogTemp,
		Verbose,
		TEXT("UnrealAI retry %d/%d in %.2f seconds"),
		RetryEvent.RetryNumber,
		RetryEvent.MaxRetries,
		RetryEvent.DelaySeconds);
}
```

The owning header must declare `HandleChatCompletion` and `HandleRetry`, include `UnrealAIClient.h`, and include the consuming module's generated header last. The implementation must include `UnrealAIBlueprintLibrary.h` and `UnrealAIProviders.h`.

Change only the factory to select XAI, Anthropic, or Gemini. For runtime selection, create the retained client with `NewObject<UUnrealAIClient>(this)` and call `ConfigureFromSettings` before submission.

## Retry and cancellation

`CreateChatCompletion` returns one handle for the logical request across all attempts. Provider policy defaults apply unless `Request.RetryOptions` disables retries or overrides the retry count. A retry event reports the same handle plus retry number, maximum, delay, reason, and HTTP status.

Call `CancelRequest(ActiveCompletion)` on the same client to cancel an active attempt or pending backoff. Successful cancellation invokes the completion callback exactly once with `Error.Code == "request_cancelled"`. Treat it as a distinct user outcome even though the one-shot error structure is populated.

The client may invoke completion synchronously for configuration or request-construction errors. Stateful owners should set pending state before submission and assign the returned handle only if the callback did not already clear that state; [multi-turn-client.md](multi-turn-client.md) shows that pattern.

## Verification

The buildable `AUnrealAISampleActor` under `Samples/UnrealAISample` exercises direct one-shot ownership. Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` for consumer compilation, and the full `Scripts/ci/run_unreal_ci.py` when changing the plugin contract. Plugin automation protects provider factories, request adapters, retries, response extraction, and cancellation behavior without provider credentials.
