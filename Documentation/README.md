# UnrealAI provider and API guide

UnrealAI is a provider-neutral Unreal Engine runtime SDK. One non-streaming chat contract drives three wire protocols:

| Profile | Wire request | Default model | API key variable |
| --- | --- | --- | --- |
| `OpenAI` | `POST {BaseUrl}/chat/completions` | `gpt-5.6-luna` | `OPENAI_API_KEY` |
| `XAI` | `POST {BaseUrl}/chat/completions` | `grok-4.6` | `XAI_API_KEY` |
| `Anthropic` | `POST {BaseUrl}/messages` | `claude-sonnet-5` | `ANTHROPIC_API_KEY` |
| `Gemini` | `POST {BaseUrl}/models/{model}:generateContent` | `gemini-3.7-flash` | `GEMINI_API_KEY` |

OpenAI and XAI share the OpenAI-compatible adapter. Anthropic and Gemini use their native request, authentication, error, response, and usage formats. The adapters normalize ordinary text conversations into `FUnrealAIChatResponse` while preserving the provider response in `RawJson`.

## Install in another project

Copy `Plugins/UnrealAI` into the target project's `Plugins` folder, then enable it in the target `.uproject`:

```json
{
  "Name": "UnrealAI",
  "Enabled": true
}
```

Regenerate project files if needed and rebuild. C++ consumers must add `UnrealAI` to the appropriate dependency list in their module's `.Build.cs`.

## Configure providers

Project Settings → Plugins → UnrealAI exposes provider profiles. A profile selects an `EUnrealAIProviderApi` protocol and supplies its base URL, default model, environment-variable names, timeout, and optional headers.

For local editor development, copy the plugin's `.env.example` to the consuming project's root as `.env`, then add only the keys needed by that project. UnrealAI loads the project-root file before resolving a profile; an existing process environment variable takes precedence.

```text
<copy command for the current host> Plugins/UnrealAI/.env.example <project root>/.env
```

Built-in optional overrides are `OPENAI_BASE_URL`/`OPENAI_MODEL`, `XAI_BASE_URL`/`XAI_MODEL`, `ANTHROPIC_BASE_URL`/`ANTHROPIC_MODEL`, and `GEMINI_BASE_URL`/`GEMINI_MODEL`. For compatibility with earlier UnrealAI releases, XAI falls back to the OpenAI base URL and model variables only when its XAI-specific variables are unset.

The `.env` loader does not replace process values it does not own. Use `Reload Project Env File` from Blueprint to reload values during editor testing.

Never store a real API key in project settings, Blueprint assets, logs, screenshots, or a packaged client. A production game should call a trusted backend that owns hosted-provider credentials.

## C++ provider factories

`UUnrealAIProviders` provides a clean provider-level creation surface:

```cpp
#include "UnrealAIBlueprintLibrary.h"
#include "UnrealAIProviders.h"

// UnrealAIClient is a UPROPERTY on this UObject so it remains alive in flight.
FUnrealAIError ConfigError;
UnrealAIClient = UUnrealAIProviders::Anthropic(this, ConfigError);
if (!UnrealAIClient)
{
    UE_LOG(LogTemp, Error, TEXT("UnrealAI configuration failed: %s"), *ConfigError.Message);
    return;
}

const FUnrealAIChatRequest Request =
    UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(
        TEXT("Summarize this level objective in one sentence."));

UnrealAIClient->CreateChatCompletion(
    Request,
    FUnrealAIChatCompletionNativeDelegate::CreateUObject(
        this,
        &ThisClass::HandleChatCompletion));
```

The built-in factories are:

- `UUnrealAIProviders::OpenAI(Outer, OutError, ModelOverride)`
- `UUnrealAIProviders::XAI(Outer, OutError, ModelOverride)`
- `UUnrealAIProviders::Anthropic(Outer, OutError, ModelOverride)`
- `UUnrealAIProviders::Gemini(Outer, OutError, ModelOverride)`

Leave the optional model override empty to use the resolved profile default. `OpenAICompatibleFromProfile` creates a client from a named compatible profile. `OpenAICompatible` accepts a complete `FUnrealAIProviderConfig` and forces the compatible Chat Completions protocol, which is useful for local model servers and gateways.

The existing `NewObject<UUnrealAIClient>(Outer)` plus `ConfigureFromSettings` flow remains supported. In either flow, retain the client in a `UPROPERTY`, handle `FUnrealAIError` before reading the response, and tolerate successful responses with no choices.

## Blueprint usage

Use `Make Simple Chat Request`, then call `Create Chat Completion (UnrealAI)`.

- `Provider Name`: `OpenAI`, `XAI`, `Anthropic`, `Gemini`, a custom profile, or empty to use the default.
- `Completed`: receives `FUnrealAIChatResponse`; use `Get First Choice Content` and branch on `Has Content`.
- `Failed`: receives `FUnrealAIError`; handle its message without exposing request or credential data.

For actor-centric gameplay, add `UnrealAIChatComponent` and call `Send Prompt`. Configure `Provider Name`, optional `Model`, `System Prompt`, and sampling values on the component. Bind `On Chat Completed` and `On Chat Failed` before sending.

![Illustrated UnrealAI Blueprint chat completion flow](Images/blueprint-chat-completion.png)

This image is an illustration, not a literal Unreal Editor capture. Add the failure branch in production graphs.

## Shared request mapping

The common text surface maps as follows:

| UnrealAI field | OpenAI compatible | Anthropic | Gemini |
| --- | --- | --- | --- |
| System/developer messages | message roles | top-level `system` blocks | `systemInstruction.parts` |
| User/assistant messages | `messages` | `messages` | `contents` with user/model roles |
| Temperature/top-p | root fields | root fields | `generationConfig` |
| Output token limit | completion/max tokens | required `max_tokens` | `maxOutputTokens` |
| Stop sequences | `stop` | `stop_sequences` | `stopSequences` |
| Choice count | supported | one only | one only |
| `ResponseFormatJson` helpers | supported | not normalized | not normalized |

`Request.Model` overrides the configured default for one request. Streaming is rejected before an HTTP request starts.

`ContentJson`, `AdditionalFieldsJson`, and `AdditionalParametersJson` remain escape hatches, but their JSON must match the selected provider's native schema. Anthropic system `ContentJson` represents system content blocks; Gemini content JSON represents one Part object or an array of Part objects. Normalized provider-independent tool calls and multimodal helpers are not available yet.

## Error and response normalization

All adapters populate `FUnrealAIError` for HTTP and provider errors. Provider error bodies remain available in `RawJson`. Successful text is normalized to `Response.Choices`; token counts are normalized to prompt, completion, and total usage fields when supplied by the provider.

Provider-native details that do not have a shared field remain in `Response.RawJson` and each choice's `RawMessageJson`. Do not log these values by default because prompts and responses may be sensitive.

## Validation

Portable validators and native Automation tests are offline and credential-free. From the plugin root, run:

```text
<python3> Scripts/ci/validate_plugin.py
<python3> Scripts/ci/validate_skills.py
<python3> Scripts/ci/validate_release.py
```

With Unreal Engine installed and `UNREAL_ENGINE_ROOT` configured for the host, run `Scripts/ci/run_unreal_ci.py --platform <Mac|Win64|Linux>`. A native run validates only its matching host platform.
