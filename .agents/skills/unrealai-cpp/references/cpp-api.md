# UnrealAI C++ API Reference

Use this reference with the public headers in the current checkout. The headers are authoritative if an API changes.

## Integration choices

| Need | API | Lifetime |
| --- | --- | --- |
| Built-in provider client | `UUnrealAIProviders` factory returning `UUnrealAIClient` | Retain as a `UPROPERTY` until callbacks finish |
| Dynamic/custom provider client | `UUnrealAIClient` configuration or compatible factory | Retain as a `UPROPERTY` until callbacks finish |
| Actor-owned prompt interface | `UUnrealAIChatComponent` | Create as a default subobject or owned component |
| One-shot completion | `UUnrealAIClient::CreateChatCompletion` | Retain the client; store its request handle when cancellation is needed |
| Incremental text stream | `UUnrealAIClient::StreamChatCompletion` | Retain the client; store its request handle when cancellation is needed |
| Request and response helpers | `UUnrealAIBlueprintLibrary` | Static functions; no retained instance |

## Module dependency

Add the plugin module to the consumer's `.Build.cs`:

```csharp
PrivateDependencyModuleNames.Add("UnrealAI");
```

Use `PublicDependencyModuleNames` instead when a public consumer header exposes an UnrealAI type.

## Provider resolution

The built-in provider factories resolve `UUnrealAISettings` and return configured clients:

- `UUnrealAIProviders::OpenAI`, `XAI`, `Anthropic`, and `Gemini`
- each accepts a valid `UObject` outer, an output error, and an optional client-level model override
- `OpenAICompatibleFromProfile` accepts a named OpenAI-compatible profile
- `OpenAICompatible` accepts a complete configuration for a compatible endpoint

`ConfigureFromSettings(ProviderName, OutError)` remains available for dynamic selection:

- `NAME_None` selects `DefaultProviderName`.
- `OpenAI`, `XAI`, `Anthropic`, and `Gemini` exist by default.
- `OPENAI_API_KEY`, `XAI_API_KEY`, `ANTHROPIC_API_KEY`, and `GEMINI_API_KEY` supply their secrets.
- Each profile has matching provider-specific base URL and model override variables. XAI falls back to `OPENAI_BASE_URL` and `OPENAI_MODEL` only when `XAI_BASE_URL` and `XAI_MODEL` are unset.
- Existing process variables take precedence over values loaded from the consuming project's root `.env` file.

Call `Configure(ProviderConfig)` when the application supplies a complete custom profile itself.

## Native request example

The client must be owned and retained while HTTP is in flight:

```cpp
// MyAIActor.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UnrealAIClient.h"
#include "MyAIActor.generated.h"

UCLASS()
class AMyAIActor : public AActor
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "AI")
    void AskUnrealAI();

private:
    UPROPERTY()
    TObjectPtr<UUnrealAIClient> UnrealAIClient;

    FUnrealAIRequestHandle ActiveCompletion;

    void HandleChatCompletion(
        const FUnrealAIChatResponse& Response,
        const FUnrealAIError& Error);
    void HandleRetry(const FUnrealAIRetryEvent& RetryEvent);
};
```

```cpp
// MyAIActor.cpp
#include "MyAIActor.h"
#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIProviders.h"

void AMyAIActor::AskUnrealAI()
{
    FUnrealAIError ConfigError;
    UnrealAIClient = UUnrealAIProviders::OpenAI(this, ConfigError);
    if (!UnrealAIClient)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealAI configuration failed: %s"), *ConfigError.Message);
        return;
    }

    const FUnrealAIChatRequest Request =
        UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(
            TEXT("Describe this level in one sentence."));

    ActiveCompletion = UnrealAIClient->CreateChatCompletion(
        Request,
        FUnrealAIChatCompletionNativeDelegate::CreateUObject(
            this,
            &AMyAIActor::HandleChatCompletion),
        FUnrealAIRetryNativeDelegate::CreateUObject(
            this,
            &AMyAIActor::HandleRetry));
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

Change only the factory name to target XAI, Anthropic, or Gemini. For a runtime-selected profile, create the retained client with `NewObject<UUnrealAIClient>(this)` and call `ConfigureFromSettings` before submitting the request.

`CreateChatCompletion` returns a handle for the entire logical request, including time spent waiting to retry. Call `CancelRequest(ActiveCompletion)` to cancel. Its completion callback then receives `Error.Code == "request_cancelled"` exactly once.

## Native streaming example

Declare a handle and two handlers on the same object that retains `UnrealAIClient`:

```cpp
FUnrealAIRequestHandle ActiveStream;
FString StreamingText;

void HandleStreamEvent(const FUnrealAIChatStreamEvent& Event);
void HandleStreamTerminal(const FUnrealAIChatStreamResult& Result);
```

Start the dedicated stream after configuring the client:

```cpp
ActiveStream = UnrealAIClient->StreamChatCompletion(
    Request,
    FUnrealAIChatStreamEventNativeDelegate::CreateUObject(
        this,
        &AMyAIActor::HandleStreamEvent),
    FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(
        this,
        &AMyAIActor::HandleStreamTerminal));

void AMyAIActor::HandleStreamEvent(const FUnrealAIChatStreamEvent& Event)
{
    if (Event.Type == EUnrealAIChatStreamEventType::TextDelta)
    {
        StreamingText += Event.TextDelta;
    }
}

void AMyAIActor::HandleStreamTerminal(const FUnrealAIChatStreamResult& Result)
{
    ActiveStream = FUnrealAIRequestHandle();

    if (Result.Status == EUnrealAIChatStreamStatus::Failed)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealAI stream failed: %s"), *Result.Error.Message);
        return;
    }

    if (Result.Status == EUnrealAIChatStreamStatus::Cancelled)
    {
        // Result.Response contains text accumulated before cancellation.
        return;
    }

    // Result.Response is the completed normalized response.
}
```

Call `UnrealAIClient->CancelRequest(ActiveStream)` on the game thread to cancel. A successful call synchronously emits the one terminal `Cancelled` result and preserves the partial normalized response. A false return means the handle was invalid, belonged to a stream that already ended, or was not active on this client.

Event callbacks arrive in provider order on the game thread. Handle `TextDelta` for incremental text, `ChoiceFinished` for finish reasons, `Usage` for normalized token counts, and `ProviderEvent` only when provider-native non-text JSON is required. The terminal callback fires exactly once with `Completed`, `Failed`, or `Cancelled`; failed and cancelled results may contain a partial response.

## Retry policy

`FUnrealAIProviderConfig::RetryPolicy` defaults to two retries, one-second initial backoff, and a 60-second delay ceiling. The delay doubles for later attempts and adds up to 25 percent jitter. `Retry-After` is a minimum delay when it fits under the configured ceiling. UnrealAI retries connection errors, timeouts, empty streams, and HTTP `408`, `409`, `429`, `500`, `502`, `503`, `504`, and `529`, except known permanent quota or billing failures.

`FUnrealAIChatRequest::RetryOptions` can use the provider policy, disable retries, or override only `MaxRetries`. Pass `FUnrealAIRetryNativeDelegate` as the final argument to either request method to observe `FUnrealAIRetryEvent` before each delay. The event reports `RequestHandle`, `RetryNumber`, `MaxRetries`, `DelaySeconds`, `Reason`, and `HttpStatus`; use `RequestHandle` to correlate concurrent logical requests.

A stream is eligible for retry only before the first complete SSE data event. Once any event has been accepted, an interruption is terminal and the result retains partial output instead of replaying the prompt and duplicating events. `CancelRequest` remains valid during backoff.

## Request surface

`FUnrealAIChatRequest` exposes a provider-neutral core:

- `Model` and `Messages`
- opt-in temperature, top-p, and token limits
- number of choices and stop sequences
- `ResponseFormatJson` for JSON object or JSON Schema output on OpenAI-compatible profiles
- `AdditionalParametersJson` for provider-specific root fields
- `RetryOptions` to inherit, disable, or override the provider retry count
- deprecated `bStream`; leave it false and select `CreateChatCompletion` or `StreamChatCompletion` explicitly

`FUnrealAIChatMessage` exposes the standard role and string content plus:

- `ContentJson` to replace string content with a raw JSON value
- `AdditionalFieldsJson` to merge provider-specific message fields
- `Name` and `ToolCallId`

Use `MakeJsonObjectResponseFormat()` or `MakeStrictJsonSchemaResponseFormat()` instead of hand-building `response_format` JSON for a compatible OpenAI endpoint. Native Anthropic and Gemini adapters accept one choice and do not normalize these response-format helpers.

System/developer messages, user/assistant text, temperature, top-p, output-token limits, and stop sequences map across all three protocols. `ContentJson`, `AdditionalFieldsJson`, and `AdditionalParametersJson` use the selected provider's native schema. Provider-independent tools, multimodal helpers, and native Anthropic/Gemini structured output are not implemented.

## Actor component pattern

Create `UUnrealAIChatComponent` as a default subobject, configure `ProviderName`, `Model`, `SystemPrompt`, optional temperature, and `RetryOptions`, then bind `OnChatCompleted`, `OnChatRetrying`, `OnChatFailed`, and `OnChatCancelled`. Call `SendPrompt` for a single user message or `SendMessages` for a prepared history. `CancelActiveCompletions` stops all one-shot requests currently owned by the component.

For streaming, also bind `OnChatStreamRetrying`, `OnChatStreamEvent`, `OnChatStreamCompleted`, `OnChatStreamFailed`, and `OnChatStreamCancelled`, then call `SendPromptStream` or `SendMessagesStream`. `CancelActiveStream` cancels the current stream. A component owns at most one active stream, so starting a second stream reports `stream_already_active` through `OnChatStreamFailed`.

Because the component owns its client, callers do not need a separate `UUnrealAIClient` property for this pattern.

## Error and response handling

- Check `Error.bIsError` first. Useful fields include `HttpStatus`, `Message`, `Type`, `Code`, `Param`, and `RawJson`.
- Use `GetFirstChoiceContent(Response, bHasContent)` for the common text path.
- Use `Response.Choices`, `Response.Usage`, or `Response.RawJson` only when the caller needs lower-level data.
- Aggregate stream results intentionally leave `Response.RawJson` empty because no single provider JSON object represents the full stream. Individual stream events may expose provider content in `RawJson`.
- Do not log authorization headers, environment values, or full request data that may contain user-sensitive content.

## Cross-platform verification

Application code should not need platform branches. For plugin validation, run the repository driver only on a matching native host:

```text
<python3> Scripts/ci/run_unreal_ci.py --platform Mac
<python3> Scripts/ci/run_unreal_ci.py --platform Win64
<python3> Scripts/ci/run_unreal_ci.py --platform Linux
```

Replace `<python3>` with the host's Python 3 launcher, such as `python3`, `python`, or `py -3`. Supply `UNREAL_ENGINE_ROOT` using that host's normal environment mechanism. Do not embed engine installation paths in source or in the skill.
