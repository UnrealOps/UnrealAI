---
name: unrealai-cpp
description: Implement, review, deploy, or debug native C++ and mixed C++/Blueprint integrations with the UnrealAI Unreal Engine plugin, including production client, dedicated-server, and backend boundaries, multi-turn state, provider configuration, chat, Responses, function tools, structured output, continuation, streaming, retries, cancellation, and module dependencies. Use for C++-owned work; use unrealai-blueprints for Blueprint-only flows.
---

# UnrealAI C++ SDK

Build against the API in the current checkout. Do not infer UnrealAI behavior from similarly named web or Unreal plugins.

## Ground the task

Treat the directory containing `UnrealAI.uplugin` as the plugin root. Inspect only the public headers that own the selected integration because the checkout may be newer than this skill:

- `Source/UnrealAI/Public/UnrealAIClient.h`
- `Source/UnrealAI/Public/UnrealAIProviders.h`
- `Source/UnrealAI/Public/UnrealAIChatComponent.h`
- `Source/UnrealAI/Public/UnrealAIChatCompletionAsyncAction.h`
- `Source/UnrealAI/Public/UnrealAIChatStreamAsyncAction.h`
- `Source/UnrealAI/Public/UnrealAIBlueprintLibrary.h`
- `Source/UnrealAI/Public/UnrealAITypes.h`
- `Source/UnrealAI/Public/UnrealAIResponseTypes.h`
- `Source/UnrealAI/Public/UnrealAIResponseLibrary.h`

Load references by integration type; do not read unrelated references:

- [references/responses-client.md](references/responses-client.md): typed Responses, tools, structured output, provider-bound continuation, and the complete compiled native example.
- [references/client-setup.md](references/client-setup.md): module dependency, provider factories, dynamic/custom profiles, request fields, and shared response/error rules.
- [references/one-shot-client.md](references/one-shot-client.md): direct `UUnrealAIClient::CreateChatCompletion` ownership, cancellation, retry, and complete example.
- [references/streaming-client.md](references/streaming-client.md): direct `StreamChatCompletion`, event/terminal contracts, and the complete compile-tested actor.
- [references/chat-component.md](references/chat-component.md): actor-component setup, stateless prompt behavior, concurrency limits, and component cancellation.
- [references/multi-turn-client.md](references/multi-turn-client.md): native conversation history, commit/rollback, bounds, and cancellation.
- [references/streaming-conversation.md](references/streaming-conversation.md): combined multi-turn streaming with one transactional owner; prefer this over loading the separate streaming and multi-turn references for that scenario.
- [references/mixed-integration.md](references/mixed-integration.md): C++/Blueprint ownership boundaries and reflected presentation surfaces.
- [references/packaged-client-deployment.md](references/packaged-client-deployment.md): player-controlled clients, listen servers, and compatible game-backend proxies.
- [references/dedicated-server-deployment.md](references/dedicated-server-deployment.md): controlled Unreal dedicated servers calling providers.
- [references/backend-deployment.md](references/backend-deployment.md): external backend policy, compatible endpoints, and streaming relays.

## Choose the integration

- Use `CreateResponse` / `StreamResponse` for typed tools, structured output, and provider-bound continuation; follow `responses-client.md` for the complete compiled example.
- Prefer `UUnrealAIProviders::OpenAI`, `XAI`, `Anthropic`, or `Gemini` to create a configured `UUnrealAIClient` for a built-in provider.
- Use `UUnrealAIClient::ConfigureFromSettings` when the provider name is selected dynamically, and `OpenAICompatible` or `OpenAICompatibleFromProfile` for custom compatible gateways.
- Use `CreateChatCompletion` for a single final response. Use `StreamChatCompletion` for incremental text and a final or partial aggregate. Both return an `FUnrealAIRequestHandle`; retain it when cancellation is needed.
- Prefer `UUnrealAIChatComponent` for an actor-owned request interface with one-shot or streaming send functions and multicast result events. The component does not retain conversation history between calls.
- Use `UUnrealAIBlueprintLibrary` helpers from C++ when they make request or response handling clearer; they are not Blueprint-only.
- For a mixed feature, choose one owner for conversation history and request state. Prefer C++ ownership when the request runs on an authoritative server. The local `AUnrealAIMultiTurnExample` exposes full SDK failure/retry structs; narrow those to allow-listed application DTOs before sending them to an untrusted client.
- Treat a listen server as an untrusted packaged client for credential decisions. Only a controlled dedicated server or backend may own a long-lived hosted-provider key.

Preserve the integration style already used by the consumer unless the user asks for a redesign.

## Required invariants

- Add `UnrealAI` to the consuming module's `PrivateDependencyModuleNames`, or to `PublicDependencyModuleNames` when UnrealAI types appear in public headers.
- Give every `UUnrealAIClient` an appropriate `UObject` outer and retain it in a `UPROPERTY` for the entire asynchronous request. A temporary unreferenced client can be garbage-collected.
- Create the client through `UUnrealAIProviders`, or call `Configure`/`ConfigureFromSettings`, before submitting any request.
- Keep common request code provider-neutral. Provider-native JSON passed through `ContentJson`, `AdditionalFieldsJson`, or `AdditionalParametersJson` must match the selected adapter.
- Treat legacy choice count and `ResponseFormatJson` as OpenAI-compatible chat features. Use `CreateResponse` / `StreamResponse` for normalized tools and structured output across adapters; media helpers remain deferred.
- Handle `FUnrealAIError` before reading the response, and tolerate a successful response with no choices.
- Leave `Request.Model` empty when the configured provider's default model is intended.
- Do not set `bStream`; it is deprecated. Choose `CreateChatCompletion` or `StreamChatCompletion` explicitly.
- Treat a request handle as one logical operation across retries and use it only with the client that returned it.
- Give conversation history, request state, and commit/rollback policy exactly one owner.
- Never hardcode, print, commit, or package API keys. A project-root `.env` is for local development. Shipped clients should call a trusted backend that owns hosted-provider secrets.
- UnrealAI does not currently enforce the production credential boundary, authenticate players, authorize requests, moderate content, impose per-player budgets, or provide a circuit breaker. Add these at the application/backend layer and document the selected deployment boundary.
- Keep ordinary integration code platform-neutral. Use Unreal abstractions such as `FPlatformMisc`, `FPaths`, and the plugin API instead of OS-specific environment or HTTP code.

## Verify the result

Match validation effort to the change:

1. Build the affected Unreal target on the current native host.
2. Run `Scripts/ci/validate_plugin.py` with the host's Python 3 launcher when that script is available.
3. For skill or recipe changes, set `UNREAL_ENGINE_ROOT` using the host's normal environment mechanism, then run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>`. This compiles an isolated sample copy, regenerates its Blueprint asset, and requires every test listed in `Scripts/ci/skill_contracts.json` to execute successfully.
4. For plugin changes, run `Scripts/ci/run_unreal_ci.py --platform <Mac|Win64|Linux>`; the full driver packages the plugin, runs its offline automation, and invokes the skill contracts.
5. Keep automated tests offline. Test request construction, retry classification and timing, SSE framing, provider event fixtures, cancellation contracts, response helpers, and error paths without real provider credentials.

If only one host is available, report which native platform was exercised rather than claiming cross-platform compilation.
