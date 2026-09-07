# C++ Client Setup and Shared Types

Use this reference when adding the module dependency, selecting or configuring a provider, or constructing non-default messages and request fields. For a simple request, also read `one-shot-client.md` or `streaming-client.md`. For a combined multi-turn stream, follow `streaming-conversation.md` directly and do not load those overlapping transport references unless changing lower-level behavior.

## Choose and retain a client

| Need | API |
| --- | --- |
| Built-in provider | `UUnrealAIProviders::OpenAI`, `XAI`, `Anthropic`, or `Gemini` |
| Runtime-selected settings profile | `UUnrealAIClient::ConfigureFromSettings` |
| Named compatible profile | `UUnrealAIProviders::OpenAICompatibleFromProfile` |
| Complete compatible configuration supplied by the application | `UUnrealAIProviders::OpenAICompatible` or `UUnrealAIClient::Configure` |

Every client needs a valid `UObject` outer and a retained `UPROPERTY` for the whole asynchronous operation. The factories return a configured client or `nullptr` plus `FUnrealAIError`. When creating a client with `NewObject<UUnrealAIClient>`, configuration must succeed before submission.

## Module dependency

Add the plugin module to the consumer's `.Build.cs`:

```csharp
PrivateDependencyModuleNames.Add("UnrealAI");
```

Use `PublicDependencyModuleNames` when an exported consumer header exposes an UnrealAI type.

## Provider resolution

The built-in factories resolve `UUnrealAISettings`; each accepts an outer, an output error, and an optional client-level model override. `ConfigureFromSettings(NAME_None, OutError)` selects `DefaultProviderName`.

The built-in credential variables are `OPENAI_API_KEY`, `XAI_API_KEY`, `ANTHROPIC_API_KEY`, and `GEMINI_API_KEY`. Each profile has corresponding base URL and model environment overrides. Inspect `Source/UnrealAI/Private/UnrealAISettings.cpp` and `.env.example` for current defaults instead of copying model names into consumer guidance. Existing process variables take precedence over values loaded from the consuming project's root `.env`.

Call `Configure(ProviderConfig)` only when the application owns the complete custom profile. Never expose long-lived provider keys to a packaged client or reflected gameplay state; use [packaged-client-deployment.md](packaged-client-deployment.md), [dedicated-server-deployment.md](dedicated-server-deployment.md), or [backend-deployment.md](backend-deployment.md) for the selected trust boundary.

## Request and message surface

`FUnrealAIChatRequest` contains:

- `Model` and `Messages`;
- opt-in temperature, top-p, output-token limits, choice count, and stop sequences;
- `ResponseFormatJson` for OpenAI-compatible JSON object or JSON Schema output;
- `AdditionalParametersJson` for selected-provider root fields;
- `RetryOptions` to inherit, disable, or override the provider retry count; and
- deprecated `bStream`, which must remain false because the method selects the transport.

`FUnrealAIChatMessage` provides role, text content, `Name`, and `ToolCallId`. `ContentJson` replaces string content with a provider-native JSON value; `AdditionalFieldsJson` merges provider-native message fields. These JSON escape hatches and `AdditionalParametersJson` must match the selected adapter.

Use `UUnrealAIBlueprintLibrary::MakeJsonObjectResponseFormat` or `MakeStrictJsonSchemaResponseFormat` rather than hand-building compatible `response_format` JSON. This legacy chat surface does not normalize choice counts or tools across Anthropic/Gemini. For typed tools and structured output, use [responses-client.md](responses-client.md); Responses supports bounded inline PNG/JPEG parts; image capture remains application-owned.

## Shared response and error rules

- Check `FUnrealAIError::bIsError` before consuming the response. Useful fields include `HttpStatus`, `Message`, `Type`, `Code`, `Param`, and `RawJson`.
- Use `GetFirstChoiceContent(Response, bHasContent)` for the common text path and handle `bHasContent == false` without indexing an empty choice array.
- Use `Response.Choices`, `Response.Usage`, or `Response.RawJson` only when lower-level data is needed.
- Leave `Request.Model` empty to inherit the configured provider default.
- Never log authorization headers, environment values, full prompts, provider bodies, or raw JSON by default.

Ordinary integration code is platform-neutral. Use the repository's native runners only on a matching Mac, Win64, or Linux host and report the platforms actually exercised.
