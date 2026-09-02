# UnrealAI

Unreal Engine runtime SDK for OpenAI-compatible generative AI providers.

The SDK starts with the portable Chat Completions contract:

- `POST {BaseUrl}/chat/completions`
- Bearer token authentication
- OpenAI-style `model`, `messages`, sampling, token, stop, and response fields
- Raw JSON extension points for provider-specific fields

The plugin is provider-neutral. The default profiles are:

- `OpenAI`: `https://api.openai.com/v1`, model `gpt-5.6-luna`, key env var `OPENAI_API_KEY`
- `XAI`: `https://api.x.ai/v1`, model `grok-4.6`, key env var `XAI_API_KEY`

Both default profiles also support `OPENAI_BASE_URL` and `OPENAI_MODEL` as environment overrides.

## Install in another project

Copy `Plugins/UnrealAI` into the target project's `Plugins` folder, then enable it in the target `.uproject`:

```json
{
  "Name": "UnrealAI",
  "Enabled": true
}
```

Regenerate project files if needed and rebuild.

## Configure providers

Project Settings -> Plugins -> UnrealAI exposes provider profiles.

Prefer environment variables for secrets:

```bash
export OPENAI_API_KEY="..."
export XAI_API_KEY="..."
```

For local editor development, copy the plugin's `.env.example` to the consuming project's root as `.env`, then add the desired keys. The SDK loads that project-root `.env` file before resolving provider settings:

```bash
cp Plugins/UnrealAI/.env.example .env
```

```env
XAI_API_KEY=...
OPENAI_BASE_URL=https://api.x.ai/v1
OPENAI_MODEL=grok-4.6
```

The `.env` loader only sets variables that are not already present in the editor process environment. Use `Reload Project Env File` from Blueprint to force a reload during editor testing.

To add another OpenAI-compatible provider, add a profile with:

- `Name`: any `FName` used from C++ or Blueprint
- `BaseUrl`: the provider API root, usually ending in `/v1`
- `BaseUrlEnvironmentVariable`: optional env var override for `BaseUrl`
- `DefaultModel`: provider model name
- `ModelEnvironmentVariable`: optional env var override for `DefaultModel`
- `ApiKeyEnvironmentVariable`: env var that stores the key
- `TimeoutSeconds`: increase for slow/reasoning models
- `AdditionalHeaders`: optional provider-specific headers

## Blueprint usage

Use `Make Simple Chat Request`, then call `Create Chat Completion (UnrealAI)`.

- `Provider Name`: `OpenAI`, `XAI`, or empty to use the default provider
- `Completed`: receives `FUnrealAIChatResponse`
- `Failed`: receives `FUnrealAIError`
- Use `Get First Choice Content` to read the assistant text from a response.

For actor-centric gameplay, add `UnrealAIChatComponent` to an actor or Blueprint and call `Send Prompt`. Configure `Provider Name`, `Model`, `System Prompt`, and optional sampling values on the component. Bind `On Chat Completed` and `On Chat Failed`.

This project includes `AInteractiveAgentAITestActor` as a simple smoke test. Place it in a level, keep `ProviderName` on its chat component as `XAI`, and call `Send Test Prompt` from Blueprint or set `Send On Begin Play` for PIE testing. Responses and failures are written to `LogInteractiveAgent`.

For multimodal/tool/provider-specific payloads:

- `FUnrealAIChatMessage.ContentJson` overrides plain string content with a raw JSON value.
- `FUnrealAIChatMessage.AdditionalFieldsJson` merges raw JSON into a message object.
- `FUnrealAIChatRequest.ResponseFormatJson` sets `response_format`.
- `FUnrealAIChatRequest.AdditionalParametersJson` merges raw JSON into the request root.

## C++ usage

```cpp
UUnrealAIClient* Client = NewObject<UUnrealAIClient>();
FUnrealAIError ConfigError;
if (!Client->ConfigureFromSettings(TEXT("XAI"), ConfigError))
{
    return;
}

FUnrealAIChatRequest Request;
Request.Messages.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
    EUnrealAIMessageRole::User,
    TEXT("Summarize this level objective in one sentence.")));

Client->CreateChatCompletion(
    Request,
    FUnrealAIChatCompletionNativeDelegate::CreateLambda(
        [](const FUnrealAIChatResponse& Response, const FUnrealAIError& Error)
        {
            if (Error.bIsError)
            {
                UE_LOG(LogTemp, Warning, TEXT("AI error: %s"), *Error.Message);
                return;
            }

            if (Response.Choices.Num() > 0)
            {
                UE_LOG(LogTemp, Log, TEXT("AI: %s"), *Response.Choices[0].Content);
            }
        }));
```

## Current scope

Implemented:

- Runtime plugin module
- Provider settings
- Non-streaming Chat Completions
- Blueprint async node
- Blueprint-spawnable chat actor component
- C++ native callback client
- Structured response and error parsing
- Raw JSON extension points

Not yet implemented:

- SSE streaming parser
- Responses API abstraction
- Image/audio/embedding convenience wrappers
- Retry/backoff policy
