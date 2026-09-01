---
name: unrealai-blueprints
description: Design, explain, review, or debug Unreal Engine Blueprint integrations with UnrealAI, including one-shot chat completions, actor chat components, provider setup, result handling, and Blueprint validation. Use for Blueprint flows; use unrealai-cpp for native-only integrations.
---

# UnrealAI Blueprints

Use the nodes exposed by the UnrealAI plugin in the current checkout. Do not substitute nodes from other AI plugins or describe planned features as available.

## Ground the task

Treat the directory containing `UnrealAI.uplugin` as the plugin root. Confirm Blueprint exposure in these headers before documenting or changing a flow:

- `Source/UnrealAI/Public/UnrealAIChatCompletionAsyncAction.h`
- `Source/UnrealAI/Public/UnrealAIChatComponent.h`
- `Source/UnrealAI/Public/UnrealAIBlueprintLibrary.h`
- `Source/UnrealAI/Public/UnrealAITypes.h`

Read [references/blueprint-api.md](references/blueprint-api.md) whenever constructing, reviewing, or explaining a graph. It defines the real node names, pins, and two supported graph patterns.

## Choose the graph pattern

- Use `Create Chat Completion (UnrealAI)` for a one-shot asynchronous request in a Level, Actor, Widget, or other Blueprint with a valid world context.
- Use `UnrealAIChatComponent` for an actor that sends repeated prompts or owns a system prompt and sampling settings.
- Use `Make Chat Message` and `Send Messages` when the caller needs roles or multiple messages. Use `Make Simple Chat Request` or `Send Prompt` for the shortest text-only path.

Preserve an existing graph's architecture unless the user asks to change it.

## Required invariants

- Handle both the success and failure execution paths. Read `FUnrealAIError.Message` on failure and do not treat an empty response as success content.
- Use `Get First Choice Content` and its `Has Content` output instead of assuming `Choices[0]` exists.
- Leave `Model` empty to inherit the selected provider profile's default model.
- Do not enable `Stream`; streaming is not implemented.
- Never put an API key in a Blueprint variable, node default, screenshot, source asset, or packaged client. Use process variables or a project-root `.env` for local editor development, and a trusted backend for shipped clients.
- Keep the graph platform-neutral. UnrealAI nodes do not require macOS-, Windows-, or Linux-specific branches.
- Label conceptual diagrams as illustrations. Do not claim that a generated diagram is a literal Unreal Editor screenshot.

## Verify the result

1. Compile the Blueprint and resolve every warning or broken pin introduced by the change.
2. Exercise `Completed` and `Failed` handling in PIE without exposing credentials in logs or screenshots.
3. For plugin changes, run the offline `UnrealAI.Blueprint.Helpers`, `UnrealAI.Blueprint.Surface`, and `UnrealAI.Response.FirstChoice` automation tests through the repository CI driver on the native host.
4. Run `Scripts/ci/validate_plugin.py` with the host's Python 3 launcher from the plugin root when available.

If a literal editor graph cannot be produced or opened, provide an exact node-and-pin recipe and clearly state that it still needs editor compilation.
