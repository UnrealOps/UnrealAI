# UnrealAI C++ API Reference

Use this reference with the public headers in the current checkout. The headers are authoritative if an API changes.

## Integration choices

| Need | API | Lifetime |
| --- | --- | --- |
| Built-in provider client | `UUnrealAIProviders` factory returning `UUnrealAIClient` | Retain as a `UPROPERTY` until callbacks finish |
| Dynamic/custom provider client | `UUnrealAIClient` configuration or compatible factory | Retain as a `UPROPERTY` until callbacks finish |
| Actor-owned prompt interface | `UUnrealAIChatComponent` | Create as a default subobject or owned component |
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

    void HandleChatCompletion(
        const FUnrealAIChatResponse& Response,
        const FUnrealAIError& Error);
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

    UnrealAIClient->CreateChatCompletion(
        Request,
        FUnrealAIChatCompletionNativeDelegate::CreateUObject(
            this,
            &AMyAIActor::HandleChatCompletion));
}

void AMyAIActor::HandleChatCompletion(
    const FUnrealAIChatResponse& Response,
    const FUnrealAIError& Error)
{
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
```

Change only the factory name to target XAI, Anthropic, or Gemini. For a runtime-selected profile, create the retained client with `NewObject<UUnrealAIClient>(this)` and call `ConfigureFromSettings` before submitting the request.

## Request surface

`FUnrealAIChatRequest` exposes a provider-neutral core:

- `Model` and `Messages`
- opt-in temperature, top-p, and token limits
- number of choices and stop sequences
- `ResponseFormatJson` for JSON object or JSON Schema output on OpenAI-compatible profiles
- `AdditionalParametersJson` for provider-specific root fields
- `bStream`, which must remain false in the current implementation

`FUnrealAIChatMessage` exposes the standard role and string content plus:

- `ContentJson` to replace string content with a raw JSON value
- `AdditionalFieldsJson` to merge provider-specific message fields
- `Name` and `ToolCallId`

Use `MakeJsonObjectResponseFormat()` or `MakeStrictJsonSchemaResponseFormat()` instead of hand-building `response_format` JSON for a compatible OpenAI endpoint. Native Anthropic and Gemini adapters accept one choice and do not normalize these response-format helpers.

System/developer messages, user/assistant text, temperature, top-p, output-token limits, and stop sequences map across all three protocols. `ContentJson`, `AdditionalFieldsJson`, and `AdditionalParametersJson` use the selected provider's native schema. Provider-independent tools, multimodal helpers, and native Anthropic/Gemini structured output are not implemented.

## Actor component pattern

Create `UUnrealAIChatComponent` as a default subobject, configure `ProviderName`, `Model`, `SystemPrompt`, and optional temperature, then bind `OnChatCompleted` and `OnChatFailed`. Call `SendPrompt` for a single user message or `SendMessages` for a prepared history.

Because the component owns its client, callers do not need a separate `UUnrealAIClient` property for this pattern.

## Error and response handling

- Check `Error.bIsError` first. Useful fields include `HttpStatus`, `Message`, `Type`, `Code`, `Param`, and `RawJson`.
- Use `GetFirstChoiceContent(Response, bHasContent)` for the common text path.
- Use `Response.Choices`, `Response.Usage`, or `Response.RawJson` only when the caller needs lower-level data.
- Do not log authorization headers, environment values, or full request data that may contain user-sensitive content.

## Cross-platform verification

Application code should not need platform branches. For plugin validation, run the repository driver only on a matching native host:

```text
<python3> Scripts/ci/run_unreal_ci.py --platform Mac
<python3> Scripts/ci/run_unreal_ci.py --platform Win64
<python3> Scripts/ci/run_unreal_ci.py --platform Linux
```

Replace `<python3>` with the host's Python 3 launcher, such as `python3`, `python`, or `py -3`. Supply `UNREAL_ENGINE_ROOT` using that host's normal environment mechanism. Do not embed engine installation paths in source or in the skill.
