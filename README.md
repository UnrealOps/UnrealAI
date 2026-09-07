# UnrealAI

[![Validate](https://github.com/UnrealOps/UnrealAI/actions/workflows/validate.yml/badge.svg)](https://github.com/UnrealOps/UnrealAI/actions/workflows/validate.yml)

⭐ Star the repository if a native AI toolkit for Unreal Engine would be useful to your project.

Build AI-powered gameplay and tools from C++ or Blueprints with one runtime API for OpenAI, xAI, Anthropic, Google Gemini, and custom OpenAI-compatible providers.

📚 See the [plugin guide](Documentation/README.md) for advanced request fields, the [production-readiness guide](Documentation/ProductionReadiness.md) for launch gaps and remediation, and the [CI guide](Documentation/ContinuousIntegration.md) for packaging and test automation.

## Table of Contents

- [About](#-about)
- [Requirements](#-requirements)
- [Installation](#-installation)
- [Quickstart](#-quickstart)
- [Sample project](#-sample-project)
- [Blueprint usage](#-blueprint-usage)
- [C++ usage](#-c-usage)
- [Provider configuration](#-provider-configuration)
- [Features and roadmap](#-features-and-roadmap)
- [Agent skills](#-agent-skills)
- [Validation](#-validation)
- [Documentation](#-documentation)
- [Contributing](#-contributing)
- [License](#-license)

## 🚀 About

UnrealAI is a provider-neutral Unreal Engine runtime plugin for adding generative AI to games, simulations, editor prototypes, and interactive experiences.

- **Blueprint and C++ APIs** — use an asynchronous Blueprint node, a spawnable actor component, or the native callback client.
- **Native provider adapters** — use OpenAI-compatible Chat Completions, Anthropic Messages, and Gemini `generateContent` behind one Unreal request/response contract.
- **Provider factories and profiles** — create `OpenAI()`, `XAI()`, `Anthropic()`, or `Gemini()` clients in C++, or select the same built-in profiles from Blueprints.
- **Environment-first configuration** — resolve API keys, base URLs, and models from process variables or a project-root `.env` file.
- **Structured output support** — request JSON objects or strict JSON Schema responses from compatible OpenAI endpoints.
- **Extensible payloads** — pass provider-native content and request fields through raw JSON extension points.
- **Runtime-only dependencies** — built on Unreal Engine's HTTP and JSON modules with no third-party runtime library.
- **Provider-neutral SSE streaming** — receive ordered text, finish, usage, and raw provider events with cancellation and an accumulated result.
- **Built-in retry and backoff** — recover from transient transport and provider failures with bounded exponential backoff, jitter, `Retry-After` support, notifications, and cancellation.

UnrealAI also exposes a native C++ execution service, typed model-provider interfaces, bounded image input, and optional authentication plugins. Agent orchestration and scene interaction belong to the consuming framework. See [Features and roadmap](#-features-and-roadmap) for the current boundaries.

## 📋 Requirements

- Unreal Engine 5.7 is the currently validated engine release.
- The platform's Unreal Engine C++ toolchain is required when compiling the plugin from source.
- An API key for the selected hosted provider, or a custom endpoint that does not require one.

## 📦 Installation

From the root of an Unreal project, clone UnrealAI into the project's `Plugins` directory:

```bash
git clone https://github.com/UnrealOps/UnrealAI.git Plugins/UnrealAI
```

Enable the plugin in the editor, or add it to the project's `.uproject` file:

```json
{
  "Name": "UnrealAI",
  "Enabled": true
}
```

Regenerate project files if needed, then rebuild the project.

For C++ consumers, add `UnrealAI` to the appropriate dependency list in the module's `.Build.cs` file:

```csharp
PrivateDependencyModuleNames.Add("UnrealAI");
```

Use a public dependency instead if UnrealAI types appear in that module's public headers.

The base SDK works independently of any agent framework or consuming application. For native providers, exact connection policies, secure storage, OAuth, and addon installation, see [Native SDK and optional authentication](Documentation/NativeSDK.md). Ordinary Blueprint and C++ callers can keep the quickstart below. Optional plugins live under `Addons/` in this repository and must be installed as sibling plugins with `Scripts/install_addons.py`.

## ⚡ Quickstart

### 1. Configure an API key

Copy the example environment file to the root of the consuming Unreal project:

```bash
cp Plugins/UnrealAI/.env.example .env
```

On PowerShell:

```powershell
Copy-Item Plugins/UnrealAI/.env.example .env
```

Add the provider key. The default OpenAI model is `gpt-5.6-luna`:

```dotenv
OPENAI_API_KEY=your_api_key_here
OPENAI_MODEL=gpt-5.6-luna
```

Add `.env` to the consuming project's `.gitignore`. Never commit API keys or place a hosted-provider key in a shipped client build. Production games should send AI requests through a trusted backend that owns the secret.

### 2. Choose an integration

- For a one-shot Blueprint request, use `Create Chat Completion (UnrealAI)`.
- For incremental Blueprint text, use `Stream Chat Completion (UnrealAI)` and retain its `Async Action` output when cancellation is needed.
- For an actor that sends repeated prompts, add an `UnrealAIChatComponent`.
- For native systems, create and retain a `UUnrealAIClient`.

### 3. Run in the editor

Start PIE and trigger the request. UnrealAI loads the project-root `.env` before resolving the provider profile. Existing process environment variables take precedence.

## 🧪 Sample project

Open the [UnrealAI sample project](Samples/UnrealAISample/README.md) for a buildable Unreal Engine 5.7 consumer with native owners, a direct-provider Blueprint, a credential-free packaged-chat Widget Blueprint, a starter level, offline contract coverage, and opt-in live provider tests. The project references this checkout through a portable relative plugin path, so it does not duplicate the plugin or require a symlink.

## 🔷 Blueprint usage

In an Actor Blueprint, connect `Make Simple Chat Request` to `Create Chat Completion (UnrealAI)`. Leave the request's model empty to use the provider profile default.

![Illustrated UnrealAI Blueprint chat completion flow](Documentation/Images/blueprint-chat-completion.png)

The illustration shows the shortest success path:

1. Create a request with `Make Simple Chat Request`.
2. Set `Provider Name` to `OpenAI`, `XAI`, `Anthropic`, `Gemini`, or leave it empty to use the default profile.
3. Connect the `Completed` response to `Get First Choice Content`.
4. Connect `Failed` to `Break UnrealAIError` in the real graph and handle its `Message` value.

For actor-centric gameplay, add an `UnrealAIChatComponent`, configure its `Provider Name`, optional `Model`, and `System Prompt`, then call `Send Prompt`. Bind `On Chat Completed` and `On Chat Failed` to receive results.

For streaming, replace the one-shot node with `Stream Chat Completion (UnrealAI)`. Append `Event.Text Delta` when `Event.Type` is `Text Delta`, use `Completed` for the normalized final response, and handle `Failed` and `Cancelled` separately. Both async nodes expose `Retrying` while waiting to try a transient failure again, plus an `Async Action` proxy whose `Cancel` function stops an active request or pending retry. Stream cancellation sends the partial response through `Cancelled`.

The chat component offers the same flow through `On Chat Retrying`, `On Chat Stream Retrying`, `Send Prompt Stream`, `On Chat Stream Event`, `On Chat Stream Completed`, `On Chat Stream Failed`, `On Chat Stream Cancelled`, and `Cancel Active Stream`. `Cancel Active Completions` stops all current one-shot requests. A component owns one active stream at a time.

## 🧩 C++ usage

Retain the client as a `UPROPERTY` while its HTTP request is active:

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
    FUnrealAIRequestHandle ActiveStream;
    FString StreamingText;

    void HandleChatCompletion(
        const FUnrealAIChatResponse& Response,
        const FUnrealAIError& Error);
    void HandleRetry(const FUnrealAIRetryEvent& RetryEvent);
};
```

Create the client, resolve a provider profile, and submit a request:

```cpp
// MyAIActor.cpp
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

Switch the factory call to `XAI`, `Anthropic`, or `Gemini` without changing the request or callback. Pass a model as the third argument to override that provider's default for the client. `OpenAICompatibleFromProfile` selects a named compatible profile, while `OpenAICompatible` accepts a complete runtime configuration.

To stream the same request, bind an incremental event callback and a terminal callback. Retain both the client and returned handle while the request is active:

```cpp
ActiveStream = UnrealAIClient->StreamChatCompletion(
    Request,
    FUnrealAIChatStreamEventNativeDelegate::CreateWeakLambda(
        this,
        [this](const FUnrealAIChatStreamEvent& Event)
        {
            if (Event.Type == EUnrealAIChatStreamEventType::TextDelta)
            {
                StreamingText += Event.TextDelta;
            }
        }),
    FUnrealAIChatStreamTerminalNativeDelegate::CreateWeakLambda(
        this,
        [this](const FUnrealAIChatStreamResult& Result)
        {
            ActiveStream = FUnrealAIRequestHandle();
            if (Result.Status == EUnrealAIChatStreamStatus::Failed)
            {
                UE_LOG(LogTemp, Error, TEXT("UnrealAI stream failed: %s"), *Result.Error.Message);
            }
        }));
```

Call `UnrealAIClient->CancelRequest(ActiveCompletion)` or `CancelRequest(ActiveStream)` to stop an active request or pending retry. One-shot cancellation completes with `Error.Code == "request_cancelled"`. A stream terminal result with status `Completed` contains the fully accumulated normalized response; `Failed` and `Cancelled` retain whatever response was accumulated first. Stream callbacks are delivered in order on the game thread.

Every provider profile defaults to two retries with a one-second initial delay and a 60-second delay ceiling. `FUnrealAIProviderConfig::RetryPolicy` configures a client, while `FUnrealAIChatRequest::RetryOptions` can inherit it, disable retries, or override the retry count for one request. Retry notifications include the logical request handle for correlation. See the [plugin guide](Documentation/README.md#retry-and-backoff) for the exact retry contract.

## 🔌 Provider configuration

UnrealAI includes these profiles by default:

| Profile | Protocol | Base URL | Default model | API key variable |
| --- | --- | --- | --- | --- |
| OpenAI | OpenAI-compatible Chat Completions | `https://api.openai.com/v1` | `gpt-5.6-luna` | `OPENAI_API_KEY` |
| XAI | OpenAI-compatible Chat Completions | `https://api.x.ai/v1` | `grok-4.6` | `XAI_API_KEY` |
| Anthropic | Anthropic Messages | `https://api.anthropic.com/v1` | `claude-sonnet-5` | `ANTHROPIC_API_KEY` |
| Gemini | Gemini `generateContent` | `https://generativelanguage.googleapis.com/v1beta` | `gemini-3.7-flash` | `GEMINI_API_KEY` |

Manage profiles under **Project Settings → Plugins → UnrealAI**. Each profile defines its API protocol, base URL, model, authentication variable, timeout, retry policy, and additional HTTP headers.

Each built-in profile recognizes provider-specific optional overrides:

```dotenv
OPENAI_BASE_URL=https://api.openai.com/v1
OPENAI_MODEL=gpt-5.6-luna
XAI_BASE_URL=https://api.x.ai/v1
XAI_MODEL=grok-4.6
ANTHROPIC_BASE_URL=https://api.anthropic.com/v1
ANTHROPIC_MODEL=claude-sonnet-5
GEMINI_BASE_URL=https://generativelanguage.googleapis.com/v1beta
GEMINI_MODEL=gemini-3.7-flash
```

For backward compatibility, the XAI profile falls back to `OPENAI_BASE_URL` and `OPENAI_MODEL` only when its `XAI_*` overrides are unset. Custom profiles can target other OpenAI-compatible providers or local gateways by selecting the OpenAI-compatible protocol. API-key overrides stored directly in project settings are supported for development, but environment variables or a trusted server are safer choices.

The legacy chat request covers text messages, system instructions, sampling, output-token limits, and stop sequences. Its choice count and `ResponseFormatJson` remain OpenAI-compatible features. New **[Responses APIs](Documentation/Responses.md)** add typed function calls/results, refusals, structured-output requests, item-level SSE events, and provider-bound continuation across OpenAI Responses, compatible Chat Completions/xAI, Anthropic Messages, and Gemini generateContent. Existing chat methods and Blueprint assets remain compatible. Reasoning/signatures and sidecar metadata are preserved as opaque provider data; inline PNG/JPEG image input is available. Audio, embeddings, and an agent executor are outside this implementation.

For a complete C++ tool round trip, use the [compiled response example](Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIResponseExample.cpp). The sample generator also creates `BP_UnrealAIResponses` and `BP_UnrealAIStreamResponses` with two actual response async nodes; the [Responses guide](Documentation/Responses.md) covers setup and validation.

## 🗺️ Features and roadmap

| Capability | Status |
| --- | --- |
| One-shot OpenAI-compatible Chat Completions | Available |
| Native Anthropic Messages | Available |
| Native Gemini `generateContent` | Available |
| C++ provider factories | Available |
| Blueprint async action | Available |
| Blueprint-spawnable chat component | Available |
| Native C++ callback client | Available |
| JSON object and strict JSON Schema response helpers | Available |
| Raw JSON request and message extensions | Available |
| Provider-neutral SSE text streaming and cancellation | Available |
| Built-in retry and backoff policy | Available |
| Responses API abstraction: typed tools, structured output, continuation | Available |
| Inline PNG/JPEG input through Responses | Available |
| UObject-free native execution and bounded model-provider SPI | Available |
| Explicit Gemini Interactions through native provider SPI | Available; convenience Gemini stays on generateContent |
| Secure stores and OAuth | Optional UnrealAIAuth plugin |
| Audio and embedding helpers | Planned |

## 🤖 Agent skills

Repository-local skills give coding agents the current UnrealAI integration rules and examples:

- [`$unrealai-cpp`](.agents/skills/unrealai-cpp/SKILL.md) — implement or review native C++ integrations.
- [`$unrealai-blueprints`](.agents/skills/unrealai-blueprints/SKILL.md) — design or review Blueprint chat flows.

Both skills keep automatic discovery enabled and route detailed work to focused references under their skill directories.

## ✅ Validation

Run portable repository checks without installing Unreal Engine:

```bash
python3 Scripts/ci/validate_plugin.py
python3 Scripts/ci/validate_skills.py
```

With a native Unreal Engine installation available, package the plugin, compile the skill examples, and run both automation suites:

```bash
UNREAL_ENGINE_ROOT=/path/to/UnrealEngine \
  python3 Scripts/ci/run_unreal_ci.py --platform Mac
```

Use `--platform Win64` on Windows or `--platform Linux` on Linux. The GitHub Actions configuration contains a manual native-host matrix for all three platforms.

For a faster recipe-only check, run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>`. Unlike the portable skill lint, this stages an isolated sample copy, builds its C++ modules, executes the Blueprint generator, reloads the generated asset, runs behavioral tests, and fails when any required contract is absent from the report.

## 📚 Documentation

- [Plugin configuration and advanced usage](Documentation/README.md)
- [Production-readiness gaps and remediation](Documentation/ProductionReadiness.md)
- [Continuous integration](Documentation/ContinuousIntegration.md)
- [Changelog](CHANGELOG.md)
- [Example environment variables](.env.example)

## 🤝 Contributing

Issues and focused pull requests are welcome. Keep changes cross-platform, add or update tests for behavioral changes, and run the portable validation before opening a pull request. Do not include `.env` files, credentials, or generated Unreal output.

## 📃 License

UnrealAI is available under the [MIT License](LICENSE).

[Back to top](#unrealai)
