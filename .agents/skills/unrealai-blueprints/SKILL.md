---
name: unrealai-blueprints
description: Design, generate, deploy, compile, explain, review, or debug Blueprint and mixed C++/Blueprint integrations with UnrealAI, including production client/server/backend boundaries, multi-turn history, chat, Responses, function tools, structured output, continuation, streaming, retries, cancellation, provider setup, result handling, and Blueprint validation. Use when Blueprint participates in the feature.
---

# UnrealAI Blueprints

Use the nodes exposed by the UnrealAI plugin in the current checkout. Do not substitute nodes from other AI plugins or describe planned features as available.

## Ground the task

Treat the directory containing `UnrealAI.uplugin` as the plugin root. Confirm Blueprint exposure only in the headers relevant to the selected graph because the checkout may be newer than this skill:

- `Source/UnrealAI/Public/UnrealAIChatCompletionAsyncAction.h`
- `Source/UnrealAI/Public/UnrealAIChatStreamAsyncAction.h`
- `Source/UnrealAI/Public/UnrealAIChatComponent.h`
- `Source/UnrealAI/Public/UnrealAIBlueprintLibrary.h`
- `Source/UnrealAI/Public/UnrealAITypes.h`
- `Source/UnrealAI/Public/UnrealAIResponseTypes.h`
- `Source/UnrealAI/Public/UnrealAIResponseLibrary.h`
- `Source/UnrealAI/Public/UnrealAIResponseAsyncAction.h`

Load references by integration type; do not read unrelated references:

- [references/responses-actions.md](references/responses-actions.md): typed response async nodes, tools, structured output, continuation, and generated two-request graphs.
- [references/async-actions.md](references/async-actions.md): one-shot and streaming async nodes, retry/cancellation pins, and response handling.
- [references/chat-component.md](references/chat-component.md): repeated actor-owned requests, component events, history behavior, and concurrency limits.
- [references/requests-and-providers.md](references/requests-and-providers.md): message/request construction, advanced fields, provider profiles, and local secrets.
- [references/asset-workflow.md](references/asset-workflow.md): literal `.uasset` generation, compilation, saving, and asset-contract testing.
- [references/multi-turn-component.md](references/multi-turn-component.md): Blueprint-owned history, commit/rollback, bounds, and streaming state.
- [references/mixed-integration.md](references/mixed-integration.md): Blueprint/C++ ownership boundaries and UI binding.
- [references/backend-streaming-conversation.md](references/backend-streaming-conversation.md): canonical packaged-client chat with a native authenticated/history controller and a generated presentation-only Widget Blueprint; prefer this combined recipe instead of loading each underlying reference.
- [references/packaged-client-deployment.md](references/packaged-client-deployment.md): player-controlled clients, listen servers, and native game-service facades.
- [references/dedicated-server-deployment.md](references/dedicated-server-deployment.md): controlled Unreal dedicated-server graphs.
- [references/backend-deployment.md](references/backend-deployment.md): Blueprint UI calling an external game backend.

## Choose the graph pattern

- Use `Create Response (UnrealAI)` / `Stream Response (UnrealAI)` for typed tools, structured output, and continuation; follow `responses-actions.md` and handle its additional `Incomplete` terminal path.
- Use `Create Chat Completion (UnrealAI)` for a one-shot asynchronous request in a Level, Actor, Widget, or other Blueprint with a valid world context. Retain its `Async Action` output when cancellation is needed.
- Use `Stream Chat Completion (UnrealAI)` when the graph needs incremental text, cancellation, or a partial aggregate after interruption.
- Use `UnrealAIChatComponent` for an actor that sends repeated independent one-shot or streaming prompts or owns system-prompt and sampling settings. The component does not retain conversation history.
- Use `Make Chat Message` and `Send Messages` when the caller needs roles or multiple messages. Use `Make Simple Chat Request` or `Send Prompt` for the shortest text-only path.

Preserve an existing graph's architecture unless the user asks to change it.

## Required invariants

- Handle both the success and failure execution paths. Read `FUnrealAIError.Message` on failure and do not treat an empty response as success content.
- Use `Get First Choice Content` and its `Has Content` output instead of assuming `Choices[0]` exists.
- Leave `Model` empty to inherit the selected provider profile's default model.
- Select `OpenAI`, `XAI`, `Anthropic`, or `Gemini` with the existing `Provider Name` pin/property; provider selection does not require provider-specific nodes.
- Treat legacy response-format helpers as OpenAI-compatible chat features. Use the new Responses nodes for typed tools and structured output across adapters; media helpers remain deferred.
- Leave the deprecated request `Stream` field disabled. Select the dedicated one-shot or streaming node instead.
- Treat `Provider Event` raw JSON as provider-specific and potentially sensitive. Do not display or log it by default.
- Give conversation history and request state exactly one owner. Do not let both a native base class and its Blueprint subclass append the same user or assistant turn.
- Never edit a `.uasset` as text or call a node recipe an editor-compiled asset. Use the asset workflow when binary content must change.
- Never put an API key in a Blueprint variable, node default, screenshot, source asset, or packaged client. Use process variables or a project-root `.env` for local editor development, and a trusted backend for shipped clients.
- Treat a listen server as an untrusted client for credentials. Authority checks control gameplay execution but do not make a player-owned process safe for a hosted-provider key.
- UnrealAI does not currently enforce the credential boundary or provide player authentication, authorization, moderation, quotas, or circuit breaking. Keep those decisions in trusted game/server code.
- Keep the graph platform-neutral. UnrealAI nodes do not require macOS-, Windows-, or Linux-specific branches.
- Label conceptual diagrams as illustrations. Do not claim that a generated diagram is a literal Unreal Editor screenshot.

## Verify the result

1. Compile the Blueprint and resolve every warning or broken pin introduced by the change.
2. Exercise all terminal paths used by the graph in PIE: `Completed`, `Failed`, and `Cancelled` for both request modes, plus `Retrying` with a controlled transient failure when practical. Do not expose credentials or raw response data in logs or screenshots.
3. For skill, recipe, or generated-asset changes, run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` on a matching Unreal-equipped host. It copies the sample to isolated output, compiles its modules, executes the generator, reloads the new Blueprint, inspects resolved nodes and pins, and requires every named behavioral contract.
4. For plugin changes, run `Scripts/ci/run_unreal_ci.py --platform <Mac|Win64|Linux>`; it packages the plugin, runs the plugin automation suite, and invokes the skill contracts.
5. Run `Scripts/ci/validate_plugin.py` with the host's Python 3 launcher from the plugin root when available.

If a literal editor graph cannot be produced or opened, provide an exact node-and-pin recipe and clearly state that it still needs editor compilation.
