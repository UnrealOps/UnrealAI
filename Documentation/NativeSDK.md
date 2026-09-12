# Native SDK and optional authentication

UnrealAI provides reusable model access. Consuming applications own agent runs, world authority, tools and effects, budgets, memory, planning, workflows, delegation, scene observation, and movement. The SDK can be installed and used independently.

## Choose an API

| Consumer | Entry point | Callback contract |
| --- | --- | --- |
| Blueprint or ordinary C++ gameplay | Existing provider factories and `UUnrealAIClient` | Game thread; validation may complete synchronously |
| Native C++ using the same convenience API | `MakeShared<FUnrealAIExecutionService, ESPMode::ThreadSafe>()`, then `Configure` | Game-thread control calls and delegates; no UObject owner needed |
| Agent runtime or custom model integration | `IUnrealAIModelProvider`, native provider configurations, `FUnrealAIModelRequest` | Native sink may be called from workers; enqueue into the consumer's owning thread |

The UObject client now delegates execution to the native convenience service. Its reflected names and factory methods remain available. The advanced provider SPI uses a bounded worker decoder and destination-bound credential transport. The convenience HTTP path remains portable; the stricter credential transport has an existing Mac implementation and reports unsupported on other platforms. These are separate SDK execution surfaces with shared access, cancellation, error classification, and retry primitives; do not assume identical transport guarantees.

`CreateChatCompletion` remains the text quickstart. `CreateResponse` and `StreamResponse` accept typed text, function tools/results, structured outputs, and provider continuation. Their inline image parts use `Type = Image`, `MimeType`, and `ImageBytes`; PNG/JPEG input is bounded and signature-checked. Native model parts additionally accept bounded GIF/WebP signatures. Image capture, access authorization, and scene metadata belong to the application.

## Native provider contracts

The SDK exports OpenAI Responses, compatible Chat Completions, Anthropic Messages, and Gemini Interactions implementations. Compatible Chat uses `FUnrealAIOpenAIResponsesProviderConfig::bUseChatCompletions`. Interactions is selected explicitly by constructing `FUnrealAIGeminiInteractionsProvider`; the convenience Gemini API continues to use **generateContent**.

Configure an exact connection snapshot and model profiles, then inject an `IUnrealAIHttpTransport`. Profiles describe normalized capabilities and limits for exact model IDs. Unknown models fail when `bRequireConfiguredModelProfile` is enabled. A profile is trusted configuration, not live evidence that a hosted model currently accepts every capability.

Responses providers send `max_output_tokens` by default. An endpoint adapter may explicitly set `FUnrealAIOpenAIResponsesProviderConfig::bSendMaxOutputTokens = false` for a compatible endpoint that rejects that field. The optional OpenAI subscription adapter uses this policy to match the [Codex request format](https://github.com/openai/codex/blob/main/codex-rs/codex-api/src/common.rs); ordinary API-key Responses and Chat Completions keep their token-limit fields. On an endpoint that omits the field, `MaxOutputTokens` is an admission value, not a provider-enforced generation cap. Deadlines, cancellation, stream byte limits, and reported usage still apply; the application must enforce its own run budget. Rebuild native provider consumers when updating the SDK's provider configuration struct.

`StartRequest` performs one physical model turn. It does not retry or execute tools. The caller must handle synchronous rejection before a handle is returned, ordered events after admission, and one logical terminal. Only `Completed` authorizes consuming accumulated tool calls or continuing a successful turn. Failed, cancelled, and timed-out streams do not authorize partial tool execution.

Opaque continuations bind provider, alias, exact credential destination, model, original history, and expected tool-output identities. Consumption happens after physical admission. They cannot be serialized as portable memory or moved across providers. Consumers add their own agent/run/world binding and validate image bytes against authorized scene observations.

Native Responses continuations retain all preceding output items and tool results, including opaque reasoning, within the bounded replay history. The original input messages appear once. The native provider snapshots this history before consuming the incoming continuation. When using the protocol helpers directly, construct the stream decoder before `Build()` or the staged continuation commit consumes it; the native provider handles that ordering automatically.

Native Responses tool schemas project inline objects and arrays into the provider's strict schema subset. Unsupported custom `format` names and `x-` annotations stay in the original application schema but are omitted from the wire copy; patterns and other constraints remain. Optional non-null fields are represented as required nullable fields on the wire. Their synthetic nulls become absent fields in completed tool calls, while required values, originally nullable values, and unknown fields remain for application validation. Such calls publish complete normalized arguments without raw argument deltas. Provider continuation history retains the exact original response arguments. Applications must still validate every completed call against their original schema and authority policy before executing it; this is not a general JSON Schema composition or reference resolver.

`FUnrealAIModelToolDescriptor::bStrict` controls provider-side constrained generation, not application authorization. A caller may explicitly set it to `false` for a route whose constrained decoder cannot handle its schemas; the SDK does not silently downgrade it. The [Codex function-tool format](https://openai.com/index/unrolling-the-codex-agent-loop/) uses `strict: false`. Always validate completed arguments against the application's original schema before executing a tool, in either mode.

## Lifetime and limits

Logical completion and physical settlement are distinct. A cancelled or timed-out operation may still own HTTP callbacks. Native request handles expose both states; convenience handles expose the optional native `Lifetime` observer. Physical capacity remains occupied until callback settlement. Keep the SDK loaded while native interfaces or continuations exist; dynamic unloading is intentionally unsupported.

The convenience defaults bound concurrent work to 32 requests, request bodies to 4 MiB, response bodies to 16 MiB, and retained stream events to 8,192. `FUnrealAIExecutionLimits` permits smaller limits while idle. Native model DTOs additionally cap aggregate retained input storage at 16 MiB; individual parts, schemas, tool calls, queues, and streams have their own bounds. Protocol encoding can reject a DTO that would exceed its wire limit.

Deadlines are monotonic and finite. The native provider includes preparation and stream processing in its absolute deadline, observes parent cancellation, and emits a terminal even when the transport never acknowledges cancellation. The engine ticker must keep running to observe an idle network timeout. Parsed event delivery also checks the deadline. No wait blocks the game thread.

The convenience service retains its bounded retry policy, including `Retry-After` and cancellation during backoff. An interrupted stream is never replayed after a complete SSE event. Shared retry math, transient/quota classification, and circuit-breaker mechanisms are in `UnrealAIAccess`; an agent runtime controls its own retry budget and selects any fallback explicitly. Do not add convenience retries underneath an agent retry loop.

Usage and safe request/error metadata are available through typed events. Prompt, response, authorization, and opaque continuation data are not diagnostic log fields.

Native HTTP failures include their numeric HTTP status in `FUnrealAIModelError::UserMessage`. Recognized parameter, model, and schema failures may add an SDK-authored explanation. Arbitrary provider error text is discarded, and malformed or oversized error bodies cannot select an explanation. Internal `http_status` metadata remains available to authorized native consumers; a public projection can display `UserMessage` without exposing response bodies or credentials.

## Optional plugins

The base plugin contains `UnrealAI`, `UnrealAIAccess`, and `UnrealAITransport`. Neither authentication nor the agent framework is required.

Install optional plugins as **sibling** Unreal plugins. From an SDK source checkout:

```text
python Scripts/install_addons.py --project /path/to/Game.uproject --auth
python Scripts/install_addons.py --project /path/to/Game.uproject --experimental
```

Packaged addons can instead be extracted directly into the project’s `Plugins` directory beside the packaged UnrealAI plugin. Enable the chosen plugin in the project. `--experimental` installs both addons. `--link` is available for development and links source inside a real sibling plugin directory; linking the entire plugin directory is unsuitable for macOS loader-relative dependencies.

- **UnrealAIAuth** supplies platform secure stores, API-key provisioning, credential brokers, browser/device OAuth, refresh coordination, durable credential transactions, and account UI. API-key setup uses a queryable configured-provider catalog. Account IDs, endpoint aliases, and token envelopes are preserved. The Mac Keychain service is `com.unrealops.unrealai`. After upgrading from a build that used a different service namespace, re-enter API keys or reconnect OAuth accounts; migration of existing credentials is manual.
- **UnrealAIExperimentalAccess** supplies the existing restricted OpenAI/xAI subscription authentication and exact resource policies. It is disabled by default and retains Server/Shipping exclusions. It does not grant subscription entitlement or change the integrations' support status.

The OpenAI subscription policy currently registers the exact `gpt-5.5` tool-capable model ID from the [Codex model catalog](https://github.com/openai/codex/blob/main/codex-rs/models-manager/models.json). Set the consumer's model ID explicitly; the older `gpt-5.6` value is not a valid replacement for that slug. A model-selection rejection does not require replacing a valid OAuth login.

That subscription policy also enables `bAllowMissingResponseContentType` because successful responses on the route may omit the header. This permits only an absent media type to reach the bounded SSE decoder. An explicit incompatible media type, malformed stream, or missing terminal event still fails. Other native providers require the SSE media type by default; enable the exception only in a trusted endpoint policy that needs it.

The route enables `bAllowEmptyTerminalOutput` for its metadata-only `response.completed` events. Full `response.output_item.done` records supply the continuation in this mode. Open calls, changed identities or arguments, missing completed items, and streams without a successful terminal still fail. A non-empty terminal output must reconcile normally. Other endpoints retain the existing terminal-output policy by default.

Base interfaces remain queryable when addons are absent: the account catalog is empty, the credential factory is unavailable, and configured providers are absent. Applications may instead inject their own credential broker and register a configured provider. No UI, login, or platform store is required for that path.

Custom auth providers should register through `IUnrealAIAccountAuthProvider::GetModularFeatureName()` and rebuild against this SDK so discovery uses the SDK-owned `UnrealAI.AccountAuthProvider` feature key.

## Clients and servers

A controlled dedicated server can supply provider credentials through an injected broker or the convenience environment configuration. A shipped game client cannot admit direct API-key/subscription credentials. Use a trusted game backend with an explicit destination, a short-lived game-session credential broker, and `GatewayBearer` / `GatewayAccounted` native policy. Gateway policy never silently rewrites a provider destination or billing identity.

`ApiKeyOverride` remains serialized for existing assets but is deprecated. Configuration getters redact credential values. Never use a serialized override or embedded provider secret for a distributed client. The SDK does not implement the game's backend, multiplayer authority, navigation, or action permissions.

## Qualification

Use the base-only, auth, and experimental consumer configurations independently. Run `UnrealAI.*` plus the consuming application’s integration tests. Native platform qualification is recorded with the build and automation reports; Mac compilation does not establish Win64/Linux transport or credential-store qualification. Live provider calls remain opt-in.
