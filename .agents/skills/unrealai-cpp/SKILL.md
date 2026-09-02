---
name: unrealai-cpp
description: Implement, review, or debug native C++ integrations with the UnrealAI Unreal Engine plugin, including provider configuration, chat requests, callbacks, structured output, and module dependencies. Use for C++ work; use unrealai-blueprints for Blueprint-only flows.
---

# UnrealAI C++ SDK

Build against the API in the current checkout. Do not infer UnrealAI behavior from similarly named web or Unreal plugins.

## Ground the task

Treat the directory containing `UnrealAI.uplugin` as the plugin root. Before changing consumer code, inspect these public contracts because the checkout may be newer than this skill:

- `Source/UnrealAI/Public/UnrealAIClient.h`
- `Source/UnrealAI/Public/UnrealAIChatComponent.h`
- `Source/UnrealAI/Public/UnrealAIBlueprintLibrary.h`
- `Source/UnrealAI/Public/UnrealAITypes.h`

Read [references/cpp-api.md](references/cpp-api.md) whenever implementing or reviewing an integration. It contains the supported API surface, ownership rules, and a complete native example.

## Choose the integration

- Prefer `UUnrealAIClient` for native services, subsystems, or request code that needs direct control over messages and callbacks.
- Prefer `UUnrealAIChatComponent` for an actor-owned conversation interface with `SendPrompt`, `SendMessages`, and multicast result events.
- Use `UUnrealAIBlueprintLibrary` helpers from C++ when they make request or response handling clearer; they are not Blueprint-only.

Preserve the integration style already used by the consumer unless the user asks for a redesign.

## Required invariants

- Add `UnrealAI` to the consuming module's `PrivateDependencyModuleNames`, or to `PublicDependencyModuleNames` when UnrealAI types appear in public headers.
- Give every `UUnrealAIClient` an appropriate `UObject` outer and retain it in a `UPROPERTY` for the entire asynchronous request. A temporary unreferenced client can be garbage-collected.
- Call `Configure` or `ConfigureFromSettings` before `CreateChatCompletion`.
- Handle `FUnrealAIError` before reading the response, and tolerate a successful response with no choices.
- Leave `Request.Model` empty when the configured provider's default model is intended.
- Do not set `bStream`; streaming is not implemented.
- Never hardcode, print, commit, or package API keys. A project-root `.env` is for local development. Shipped clients should call a trusted backend that owns hosted-provider secrets.
- Keep ordinary integration code platform-neutral. Use Unreal abstractions such as `FPlatformMisc`, `FPaths`, and the plugin API instead of OS-specific environment or HTTP code.

## Verify the result

Match validation effort to the change:

1. Build the affected Unreal target on the current native host.
2. Run `Scripts/ci/validate_plugin.py` with the host's Python 3 launcher when that script is available.
3. For plugin changes, set `UNREAL_ENGINE_ROOT` using the host's normal environment mechanism, then run `Scripts/ci/run_unreal_ci.py --platform <Mac|Win64|Linux>` with Python 3 on the matching host.
4. Keep automated tests offline. Test request construction, settings, response helpers, and error paths without real provider credentials.

If only one host is available, report which native platform was exercised rather than claiming cross-platform compilation.
