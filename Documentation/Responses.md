# Provider-neutral Responses

Use `CreateResponse` / `StreamResponse` for typed text, refusals, function calls, tool results, structured-output requests, and provider-bound continuation. These are additive APIs: `CreateChatCompletion`, `StreamChatCompletion`, their Blueprint nodes, and `UUnrealAIChatComponent` retain their existing contracts. UnrealAI supplies the model transport and data model, not an agent executor.

## Protocol selection and capabilities

`FUnrealAIProviderConfig::Api` still selects the chat protocol. The new `ResponseApi` applies **only** to the Responses methods:

| ResponseApi | Behavior |
| --- | --- |
| `ProviderDefault` | Official `OpenAI` profile at `https://api.openai.com/v1` uses native Responses; other profiles use their configured chat protocol. |
| `ChatProtocol` | Use the existing compatible Chat Completions, Anthropic Messages, or Gemini generateContent protocol. |
| `OpenAIResponses` | Explicitly target `/responses` on an OpenAI-compatible profile, including a custom gateway. |

Changing a base URL never probes an endpoint, falls back after submitting a request, or silently moves an existing chat call to Responses. Custom compatible profiles must opt into native Responses explicitly.

| Adapter | Text / SSE / function tools | JSON object | JSON Schema | Strict function schema flag | Stored continuation |
| --- | --- | --- | --- | --- | --- |
| OpenAI Responses | Yes | Yes | Yes | Yes | Opt-in |
| Compatible Chat Completions, including xAI | Yes | Yes | Yes | Yes | No |
| Anthropic Messages | Yes | No | Yes | Yes | No |
| Gemini generateContent | Yes | Yes | Yes | No | No |

These are **adapter capabilities**, not a promise that every model or compatible server supports every option. Query `Client->GetResponseCapabilities()` and use `ValidateResponseRequest(Request, Error)` for credential-free construction validation. Model-specific restrictions, JSON Schema dialects, token budgets, and provider policy are still enforced by the endpoint. Schema JSON is syntax-checked, not fully validated against each provider's schema dialect.

All adapters support automatic, disabled, required, and named tool choices. A named choice must reference a supplied tool. Gemini rejects strict function flags; Anthropic rejects JSON-object-only mode. There is exactly one generation per request; use the legacy chat API for compatible multi-choice requests.

## Native C++

Read the complete compiled [header](../Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIResponseExample.h) and [implementation](../Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIResponseExample.cpp) for client ownership, cancellation, synchronous preflight, retry/event handling, and a bounded two-request tool round trip.

Place `AUnrealAIResponseExample` in a trusted standalone/server world, select a configured `ProviderName`, then call:

- `StartNativeResponses(false)` for a final response per request.
- `StartNativeResponses(true)` for incremental events plus a final response.
- `CancelResponses()` to cancel the active logical request, including backoff.

The sample requires exactly one `get_level_name` tool call, validates its empty-object arguments, reads the level name, builds the continuation, disables further tool calls, and stops after the second request. It does not loop automatically. Adapt its module API macro and retain the `UnrealAI` dependency when copying it.

For a text-only request, `UUnrealAIResponseLibrary::MakeResponseRequest(Prompt)` creates one user message. Submit it through a configured, `UPROPERTY`-retained `UUnrealAIClient` using `FUnrealAIResponseNativeDelegate`. Streaming additionally takes `FUnrealAIResponseEventNativeDelegate`. Both accept the existing `FUnrealAIRetryNativeDelegate` and return `FUnrealAIRequestHandle`.

## Request and output model

`FUnrealAIResponseRequest` includes:

- `Instructions` for system guidance; `Input` is an ordered array of user/assistant messages or tool results.
- Optional model, sampling, output-token limits, and existing retry overrides.
- `Tools` containing name, description, parameter-schema JSON, and an opt-in strict flag.
- `ToolChoice` / `NamedTool`, and `OutputFormat` (`Text`, `JsonObject`, `JsonSchema`) with schema/name/strict fields.
- Local `History`, or native OpenAI `bStore` plus optional `PreviousResponseId`.
- `AdditionalParametersJson` for provider-native root fields that the SDK does not own.

Extensions cannot replace input, instructions, model, tools, sampling, output format, generation count, stream selection, storage, continuation, background execution, or the encrypted-reasoning include list. Invalid requests fail before HTTP submission. Leave the model empty to use the selected profile default.

`FUnrealAIResponse::Output` contains ordered `Message`, `ToolCall`, or `ProviderData` items. Messages contain `Text`, `Refusal`, or `ProviderData` parts. Tool calls expose call ID, tool name, and argument JSON separately from text. Unknown blocks, reasoning data, signatures, citations, grounding, safety ratings, and usage extensions remain in item/part/response `RawJson` or raw stream events; these do not all have dedicated normalized types.

`GetResponseText` concatenates only visible message text, excluding refusals, reasoning, and tool arguments. A completed response can legitimately contain no visible text. Inspect refusal parts and `FinishReason` instead of assuming an empty text string is an error.

## Status, streaming, and cancellation

Every accepted request delivers one terminal `FUnrealAIResponseResult` on the game thread. Preflight failure can invoke it synchronously and return an invalid handle.

| Status | Consumer action |
| --- | --- |
| `Completed` | Read text/refusal/output items; validate eligible tool calls before executing any application action. |
| `Incomplete` | Preserve partial display output and inspect `IncompleteReason`; do not execute partial tools or continue automatically. |
| `Failed` | Inspect the normalized error and optionally retain partial output; no automatic continuation. |
| `Cancelled` | Treat as user cancellation, with an empty error and any accumulated partial output. |

Streaming exposes `ItemAdded`, `PartAdded`, `TextDelta`, `RefusalDelta`, `ToolArgumentsDelta`, `ItemCompleted`, `Usage`, and `ProviderEvent`. Events carry the logical request handle, output index, part index where relevant, and a stable item ID. Tool call IDs may become available after item creation. Gemini retains individual source parts rather than merging signed text parts.

`ProviderEvent` is emitted for every JSON source frame so sidecar metadata is not lost when a frame also contains normalized text. Do not append raw events to visible text. Treat all raw data as potentially sensitive.

Final snapshots reconcile against deltas and only emit missing suffixes; consumers should assign the final aggregate text, not append it. A stream without the required provider terminal marker fails, retaining partial output. Cancellation clears HTTP/delegate/ticker ownership. Reconfiguration or UObject destruction follows the existing client contract: active operations are torn down without invoking application callbacks.

Retries reuse the existing bounded policy and logical handle. **After the first complete SSE data event, including a lifecycle-only event, the request is never replayed.** Arguments are bounded to 1 MiB per tool, SSE events to 1 MiB, and queued transport data to 4 MiB. Total transcript/response size and active-request limits remain application responsibilities.

## Tool results and continuation

1. Wait for `Completed`, then call `GetResponseToolCalls(Result)`. It returns only complete, syntactically valid object arguments.
2. Check the tool name against an allowlist, validate its schema and application permissions, and apply deadlines, budgets, and idempotency before executing it. The SDK does not execute functions or validate gameplay authorization.
3. Call `MakeToolResult(Call, Output, bIsError)` for every pending call, exactly once.
4. Use `BuildContinuationRequest(PreviousRequest, PreviousResult, NewInput, NextRequest, Error)`.
5. Submit `NextRequest` only if the helper succeeds; explicitly bound the number of tool/model steps.

The helper rejects missing, duplicate, unmatched, or misnamed tool results. It places tool results before any new messages. Anthropic/Gemini parallel results are grouped into a single user message. Tool errors use Anthropic's native flag, Gemini's error response, or an `error` JSON envelope in compatible/native OpenAI tool-output text.

Local continuation is the default. It replays the complete original provider output, including encrypted OpenAI reasoning and Anthropic/Gemini signatures. The opaque history is bound to the provider profile, endpoint, protocol, model, and OpenAI organization/project. Do not edit it, reconstruct it from visible text, share it across players, or send it to a different provider. Keep one history owner and independent request snapshots for concurrent conversations. Drop whole conversations when changing providers/models; there is no automatic history translation.

This binding detects configuration mistakes; it is not a signature, authorization token, or safe way to accept untrusted client-supplied transcripts.

For native OpenAI only, `bStore=true` opts into provider storage. The helper then sends just new input plus `PreviousResponseId`, not a duplicate local transcript. Re-send instructions and tool definitions through the copied request. Retain the matching previous request/result pair. Storage mode cannot be changed mid-continuation. The local-mode `store=false` setting is not a universal zero-retention guarantee for other providers or infrastructure.

## Blueprints

Use `Create Response (UnrealAI)` or `Stream Response (UnrealAI)`. Both expose `Completed`, `Incomplete`, `Failed`, `Cancelled`, and `Retrying`; streaming uses the additional `Event` path. Retain the `Async Action` output and call `Cancel` for cancellation.

All async paths share one reflected delegate signature so Unreal exposes `Result`, `Response Event`, and `Retry Event` data pins. Read only the pin matching the active execution path; the other two contain empty defaults. The proxy pin's internal name is `AsyncTaskProxy` (its friendly label is `AsyncAction`).

The sample generator produces real, compiled `BP_UnrealAIResponses` and `BP_UnrealAIStreamResponses` assets under `/Game/Blueprints` in the staged sample. Each overrides `StartBlueprintResponses`, uses two actual response async nodes, branches on preparation/continuation validation, and handles every terminal pin. The native base class owns the allowlisted gameplay tool and request snapshots; the graph owns submission. This is a mixed integration, not automatic tool execution in the plugin.

For Blueprint-owned tools, use the reflected `Make Response Request`, `Make Tool Definition`, `Get Response Text`, `Get Response Tool Calls`, `Make Tool Result`, and `Build Continuation Request` helpers with the same validation and trust-boundary rules. The chat component is still the legacy text interface; it does not acquire Responses history implicitly.

## Validation and boundaries

Run the [native CI driver](ContinuousIntegration.md) on a matching Mac, Win64, or Linux host. Its skill phase:

- compiles the exact C++ sample;
- generates, compiles, saves, and reloads both response Blueprint assets;
- uses a temporary loopback-only Python HTTP fixture, without provider keys;
- executes native and Blueprint one-shot/streamed two-step tool exchanges over all four wire protocols;
- tests retry success, no replay after a lifecycle frame, cancellation, and game-thread/exactly-once terminal delivery;
- requires the response request, continuation, streaming, safety, and legacy regression suites in the manifest.

These fixtures verify SDK contracts, not live provider/model availability. Strict native compilation on one platform does not certify another. Native Windows/Linux runs still require their respective Unreal-equipped hosts.

Deferred: hosted tools, automatic agent loops, media/upload helpers, embeddings, Gemini Interactions, background/polling APIs, persistent cross-provider memory, schema-based game-action authorization, and comprehensive global resource budgets.

Protocol references: [OpenAI Responses migration](https://developers.openai.com/api/docs/guides/migrate-to-responses), [OpenAI function calling](https://developers.openai.com/api/docs/guides/function-calling), [Anthropic tool results](https://platform.claude.com/docs/en/agents-and-tools/tool-use/handle-tool-calls), [Anthropic structured output](https://platform.claude.com/docs/en/build-with-claude/structured-outputs), [Gemini REST schema](https://ai.google.dev/api/generate-content), and [Gemini thought signatures](https://ai.google.dev/gemini-api/docs/generate-content/thought-signatures).
