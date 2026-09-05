# Blueprint Requests and Provider Setup

Use this reference when constructing messages, using advanced request fields, selecting a provider profile, or configuring local editor credentials. Read `async-actions.md` or `chat-component.md` separately for execution flow.

## Build requests

`Make Simple Chat Request` creates a one-user-message request. Leave `Model` empty to inherit the selected profile's default. For multi-message requests, create `UnrealAIChatMessage` values with `Make Chat Message`; supported roles are `System`, `Developer`, `User`, `Assistant`, and `Tool`.

The request's `Stream` field is deprecated. Keep it disabled and select the dedicated one-shot or streaming node or component function.

Expand advanced pins only when required:

- `Content Json` replaces string content with a provider-native JSON value.
- `Additional Fields Json` merges provider-native fields into one message.
- `Response Format Json` sets `response_format` for OpenAI-compatible profiles.
- `Additional Parameters Json` merges provider-native fields into the request root.
- `Retry Options` inherits provider policy, disables retries, or overrides only the maximum retry count.

Invalid JSON fails request construction. Prefer `Make Json Object Response Format` or `Make Strict Json Schema Response Format` for compatible OpenAI endpoints. Anthropic and Gemini normalize core text chat but do not currently normalize choice counts, tools, multimodal helpers, or these structured-output helpers.

## Select providers

Set `Provider Name` to `OpenAI`, `XAI`, `Anthropic`, `Gemini`, a custom settings profile, or `None` for the project default. Provider selection does not require provider-specific request nodes.

Profiles live under **Project Settings → Plugins → UnrealAI**. Use `Resolve Provider Config` only when the graph needs to inspect resolution success and redacted configuration metadata. Use `Reload Project Env File` only for local editor iteration.

Inspect `.env.example` and `Source/UnrealAI/Private/UnrealAISettings.cpp` for current environment-variable names, endpoints, and model defaults rather than copying those values into assets or secondary documentation.

## Credentials

For local editor development, copy `.env.example` to `.env` in the consuming project root; process variables take precedence. Never put a real key in a Blueprint variable, pin default, asset, screenshot, source-controlled setting, save game, or packaged client.

Blueprint does not provide secure storage for long-lived provider credentials. Read [packaged-client-deployment.md](packaged-client-deployment.md), [dedicated-server-deployment.md](dedicated-server-deployment.md), or [backend-deployment.md](backend-deployment.md) for the runtime that will execute the graph.

## Verification

Compile after changing request pins. Exercise invalid JSON and a valid no-choice response, and verify failure and `Has Content == false` are handled. Repository automation protects request helpers, provider resolution, default profiles, and Blueprint reflection without real keys.
