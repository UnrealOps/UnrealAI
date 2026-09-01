# UnrealAI

[![Validate](https://github.com/UnrealOps/UnrealAI/actions/workflows/validate.yml/badge.svg)](https://github.com/UnrealOps/UnrealAI/actions/workflows/validate.yml)

⭐ Star the repository if a native AI toolkit for Unreal Engine would be useful to your project.

Build AI-powered gameplay and tools from C++ or Blueprints with one runtime API for OpenAI-compatible Chat Completions providers.

📚 See the [plugin guide](Documentation/README.md) for advanced request fields and the [CI guide](Documentation/ContinuousIntegration.md) for packaging and test automation.

## Table of Contents

- [About](#-about)
- [Requirements](#-requirements)
- [Installation](#-installation)
- [Quickstart](#-quickstart)
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
- **Unified provider profiles** — switch between OpenAI, xAI, local gateways, and other OpenAI-compatible APIs without rewriting gameplay code.
- **Environment-first configuration** — resolve API keys, base URLs, and models from process variables or a project-root `.env` file.
- **Structured output support** — request JSON objects or strict JSON Schema responses.
- **Extensible payloads** — add multimodal content and provider-specific fields through raw JSON extension points.
- **Runtime-only dependencies** — built on Unreal Engine's HTTP and JSON modules with no third-party runtime library.

UnrealAI currently implements non-streaming `POST {BaseUrl}/chat/completions` requests. See [Features and roadmap](#-features-and-roadmap) for the current boundaries.

## 📋 Requirements

- Unreal Engine 5.7 is the currently validated engine release.
- The platform's Unreal Engine C++ toolchain is required when compiling the plugin from source.
- An API key for the selected hosted provider, or an OpenAI-compatible endpoint that does not require one.

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
- For an actor that sends repeated prompts, add an `UnrealAIChatComponent`.
- For native systems, create and retain a `UUnrealAIClient`.

### 3. Run in the editor

Start PIE and trigger the request. UnrealAI loads the project-root `.env` before resolving the provider profile. Existing process environment variables take precedence.

## 🔷 Blueprint usage

In an Actor Blueprint, connect `Make Simple Chat Request` to `Create Chat Completion (UnrealAI)`. Leave the request's model empty to use the provider profile default.

![Illustrated UnrealAI Blueprint chat completion flow](Documentation/Images/blueprint-chat-completion.png)

The illustration shows the shortest success path:

1. Create a request with `Make Simple Chat Request`.
2. Set `Provider Name` to `OpenAI`, `XAI`, or leave it empty to use the default profile.
3. Connect the `Completed` response to `Get First Choice Content`.
4. Connect `Failed` to `Break UnrealAIError` in the real graph and handle its `Message` value.

For actor-centric gameplay, add an `UnrealAIChatComponent`, configure its `Provider Name`, optional `Model`, and `System Prompt`, then call `Send Prompt`. Bind `On Chat Completed` and `On Chat Failed` to receive results.

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

    void HandleChatCompletion(
        const FUnrealAIChatResponse& Response,
        const FUnrealAIError& Error);
};
```

Create the client, resolve a provider profile, and submit a request:

```cpp
// MyAIActor.cpp
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

## 🔌 Provider configuration

UnrealAI includes these profiles by default:

| Provider | Base URL | Default model | API key variable |
| --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1` | `gpt-5.6-luna` | `OPENAI_API_KEY` |
| xAI | `https://api.x.ai/v1` | `grok-4.6` | `XAI_API_KEY` |

Manage profiles under **Project Settings → Plugins → UnrealAI**. Each profile can define its base URL, model, authentication variable, timeout, and additional HTTP headers.

Both built-in profiles recognize these optional overrides:

```dotenv
OPENAI_BASE_URL=https://api.openai.com/v1
OPENAI_MODEL=gpt-5.6-luna
```

Custom profiles can target other OpenAI-compatible providers or local gateways. API-key overrides stored directly in project settings are supported for development, but environment variables or a trusted server are safer choices.

## 🗺️ Features and roadmap

| Capability | Status |
| --- | --- |
| Non-streaming Chat Completions | Available |
| Blueprint async action | Available |
| Blueprint-spawnable chat component | Available |
| Native C++ callback client | Available |
| JSON object and strict JSON Schema response helpers | Available |
| Raw JSON request and message extensions | Available |
| SSE streaming | Planned |
| Responses API abstraction | Planned |
| Image, audio, and embedding helpers | Planned |
| Built-in retry and backoff policy | Planned |

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

With a native Unreal Engine installation available, package the plugin and run its automation tests:

```bash
UNREAL_ENGINE_ROOT=/path/to/UnrealEngine \
  python3 Scripts/ci/run_unreal_ci.py --platform Mac
```

Use `--platform Win64` on Windows or `--platform Linux` on Linux. The GitHub Actions configuration contains a manual native-host matrix for all three platforms.

## 📚 Documentation

- [Plugin configuration and advanced usage](Documentation/README.md)
- [Continuous integration](Documentation/ContinuousIntegration.md)
- [Example environment variables](.env.example)

## 🤝 Contributing

Issues and focused pull requests are welcome. Keep changes cross-platform, add or update tests for behavioral changes, and run the portable validation before opening a pull request. Do not include `.env` files, credentials, or generated Unreal output.

## 📃 License

UnrealAI is available under the [MIT License](LICENSE).

[Back to top](#unrealai)
