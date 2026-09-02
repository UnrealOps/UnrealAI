# UnrealAI C++ API Reference

Use this reference with the public headers in the current checkout. The headers are authoritative if an API changes.

## Integration choices

| Need | API | Lifetime |
| --- | --- | --- |
| Direct native requests | `UUnrealAIClient` | Retain as a `UPROPERTY` until callbacks finish |
| Actor-owned prompt interface | `UUnrealAIChatComponent` | Create as a default subobject or owned component |
| Request and response helpers | `UUnrealAIBlueprintLibrary` | Static functions; no retained instance |

## Module dependency

Add the plugin module to the consumer's `.Build.cs`:

```csharp
PrivateDependencyModuleNames.Add("UnrealAI");
```

Use `PublicDependencyModuleNames` instead when a public consumer header exposes an UnrealAI type.

## Provider resolution

`ConfigureFromSettings(ProviderName, OutError)` resolves `UUnrealAISettings`:

- `NAME_None` selects `DefaultProviderName`.
- `OpenAI` and `XAI` exist by default.
- `OPENAI_API_KEY` and `XAI_API_KEY` supply the built-in profile secrets.
- `OPENAI_BASE_URL` and `OPENAI_MODEL` can override the corresponding fields of either built-in profile.
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

void AMyAIActor::AskUnrealAI()
{
    UnrealAIClient = NewObject<UUnrealAIClient>(this);

    FUnrealAIError ConfigError;
    if (!UnrealAIClient->ConfigureFromSettings(TEXT("OpenAI"), ConfigError))
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

## Request surface

`FUnrealAIChatRequest` exposes:

- `Model` and `Messages`
- opt-in temperature, top-p, and token limits
- number of choices and stop sequences
- `ResponseFormatJson` for JSON object or JSON Schema output
- `AdditionalParametersJson` for provider-specific root fields
- `bStream`, which must remain false in the current implementation

`FUnrealAIChatMessage` exposes the standard role and string content plus:

- `ContentJson` to replace string content with a raw JSON value
- `AdditionalFieldsJson` to merge provider-specific message fields
- `Name` and `ToolCallId`

Use `MakeJsonObjectResponseFormat()` or `MakeStrictJsonSchemaResponseFormat()` instead of hand-building `response_format` JSON when those formats meet the requirement.

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
