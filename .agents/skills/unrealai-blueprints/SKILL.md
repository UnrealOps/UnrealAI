---
name: unrealai-blueprints
description: Design, explain, review, or debug Unreal Engine Blueprint integrations with UnrealAI, including one-shot or streaming chat, retries, cancellation, actor chat components, provider setup, result handling, and Blueprint validation. Use for Blueprint flows; use unrealai-cpp for native-only integrations.
---

# UnrealAI Blueprints

Use the nodes exposed by the UnrealAI plugin in the current checkout. Do not substitute nodes from other AI plugins or describe planned features as available.

## Ground the task

Treat the directory containing `UnrealAI.uplugin` as the plugin root. Confirm Blueprint exposure in these headers before documenting or changing a flow:

- `Source/UnrealAI/Public/UnrealAIChatCompletionAsyncAction.h`
- `Source/UnrealAI/Public/UnrealAIChatStreamAsyncAction.h`
- `Source/UnrealAI/Public/UnrealAIChatComponent.h`
- `Source/UnrealAI/Public/UnrealAIBlueprintLibrary.h`
- `Source/UnrealAI/Public/UnrealAITypes.h`

Read [references/blueprint-api.md](references/blueprint-api.md) whenever constructing, reviewing, or explaining a graph. It defines the real node names, pins, and supported one-shot, streaming, and component patterns.

## Choose the graph pattern

- Use `Create Chat Completion (UnrealAI)` for a one-shot asynchronous request in a Level, Actor, Widget, or other Blueprint with a valid world context. Retain its `Async Action` output when cancellation is needed.
- Use `Stream Chat Completion (UnrealAI)` when the graph needs incremental text, cancellation, or a partial aggregate after interruption.
- Use `UnrealAIChatComponent` for an actor that sends repeated one-shot or streaming prompts or owns a system prompt and sampling settings.
- Use `Make Chat Message` and `Send Messages` when the caller needs roles or multiple messages. Use `Make Simple Chat Request` or `Send Prompt` for the shortest text-only path.

Preserve an existing graph's architecture unless the user asks to change it.

## Required invariants

- Handle both the success and failure execution paths. Read `FUnrealAIError.Message` on failure and do not treat an empty response as success content.
- Use `Get First Choice Content` and its `Has Content` output instead of assuming `Choices[0]` exists.
- Leave `Model` empty to inherit the selected provider profile's default model.
- Select `OpenAI`, `XAI`, `Anthropic`, or `Gemini` with the existing `Provider Name` pin/property; provider selection does not require provider-specific nodes.
- Treat the response-format helper nodes as OpenAI-compatible features. Anthropic and Gemini currently normalize core text chat, not provider-independent tools, multimodal helpers, or structured output.
- Leave the deprecated request `Stream` field disabled. Select the dedicated one-shot or streaming node instead.
- Leave `Retry Options` in `Use Provider Policy` unless this request intentionally disables retries or overrides their count. Use `Retrying` only for progress or telemetry; it is not a terminal path. Its retry event carries the logical request handle for correlating concurrent component requests.
- Handle one-shot `Cancelled` separately from `Failed`. The inherited `Cancel` function stops an active attempt or pending retry.
- On a stream, append only `Text Delta` events to user-visible incremental text. Handle `Completed`, `Failed`, and `Cancelled` separately; failure and cancellation may carry a partial response.
- A stream retries only before its first complete SSE data event. Never reconstruct a Blueprint replay loop after partial output because it can duplicate text or provider events.
- Retain the streaming node's exposed `Async Action` output when cancellation is needed, and invoke its inherited `Cancel` function. For a component, use `Cancel Active Stream`.
- Treat `Provider Event` raw JSON as provider-specific and potentially sensitive. Do not display or log it by default.
- Never put an API key in a Blueprint variable, node default, screenshot, source asset, or packaged client. Use process variables or a project-root `.env` for local editor development, and a trusted backend for shipped clients.
- Keep the graph platform-neutral. UnrealAI nodes do not require macOS-, Windows-, or Linux-specific branches.
- Label conceptual diagrams as illustrations. Do not claim that a generated diagram is a literal Unreal Editor screenshot.

## Verify the result

1. Compile the Blueprint and resolve every warning or broken pin introduced by the change.
2. Exercise all terminal paths used by the graph in PIE: `Completed`, `Failed`, and `Cancelled` for both request modes, plus `Retrying` with a controlled transient failure when practical. Do not expose credentials or raw response data in logs or screenshots.
3. For plugin changes, run the offline `UnrealAI.Blueprint.Helpers`, `UnrealAI.Blueprint.Surface`, `UnrealAI.Response.FirstChoice`, `UnrealAI.Retry.Policy`, `UnrealAI.Streaming.SseParser`, and `UnrealAI.Streaming.ProviderAdapters` automation tests through the repository CI driver on the native host.
4. Run `Scripts/ci/validate_plugin.py` with the host's Python 3 launcher from the plugin root when available.

If a literal editor graph cannot be produced or opened, provide an exact node-and-pin recipe and clearly state that it still needs editor compilation.
