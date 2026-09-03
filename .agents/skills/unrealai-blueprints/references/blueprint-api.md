# UnrealAI Blueprint API Reference

Use this reference with the Blueprint-exposed declarations in the current checkout. The public headers are authoritative if a node changes.

## One-shot async flow

Use this pattern for one request:

1. Add `Make Simple Chat Request` and enter the prompt. Leave `Model` empty to use the provider default.
2. Add `Create Chat Completion (UnrealAI)` and connect the request value.
3. Set `Provider Name` to `OpenAI`, `XAI`, `Anthropic`, `Gemini`, a custom profile name, or `None` for the default profile.
4. Connect the async node's `Completed` execution path and `Response` value to `Get First Choice Content`.
5. Branch on `Has Content` before consuming the returned string.
6. Connect `Failed`, break the supplied `UnrealAIError`, and display or handle `Message` without exposing secrets.

The README illustration at `Documentation/Images/blueprint-chat-completion.png` shows the compact success path. It is an illustration, not a literal editor capture; add the failure branch in production graphs.

## Streaming async flow

Use this pattern when the graph needs incremental text or cancellation:

1. Add `Make Simple Chat Request`, leave its deprecated `Stream` field disabled, and connect it to `Stream Chat Completion (UnrealAI)`.
2. Select `Provider Name` exactly as for the one-shot node.
3. Promote the exposed `Async Action` output to a variable when the graph needs to call `Cancel` later.
4. On `Event`, switch on `Event.Type`. Append `Event.Text Delta` only for `Text Delta`; use `Choice Finished` and `Usage` when their normalized values matter.
5. Handle `Completed` as the full aggregate response and `Failed` as an error plus any partial response.
6. Handle `Cancelled` separately. Its `Partial Response` contains any normalized text received before cancellation.

`Provider Event` exposes native non-text or otherwise unnormalized JSON. Treat it as provider-specific, potentially sensitive data and do not print it by default. The node emits stream events in order and exactly one terminal path.

## Actor component flow

Use this pattern for repeated actor-owned prompts:

1. Add an `UnrealAIChatComponent` to the Actor Blueprint.
2. Set `Provider Name`, optional `Model`, optional `System Prompt`, and optional temperature in the Details panel.
3. For one-shot work, bind `On Chat Completed` and `On Chat Failed`, then call `Send Prompt` or `Send Messages`.
4. For streaming, bind `On Chat Stream Event`, `On Chat Stream Completed`, `On Chat Stream Failed`, and `On Chat Stream Cancelled` before calling `Send Prompt Stream` or `Send Messages Stream`.
5. Call `Cancel Active Stream` when the current component stream should stop.
6. Use `Get First Choice Content` on complete or partial responses and inspect `UnrealAIError.Message` on failures.

The component creates and retains its internal client. Do not create a second client for the same component flow. It owns at most one active stream; a second start fails with `stream_already_active` without cancelling the existing stream.

## Node catalog

| Blueprint display name | Purpose | Important outputs |
| --- | --- | --- |
| `Make Chat Message` | Create a role and content pair | `UnrealAIChatMessage` |
| `Make Simple Chat Request` | Create a one-user-message request | `UnrealAIChatRequest` |
| `Create Chat Completion (UnrealAI)` | Send an asynchronous request | `Completed`, `Failed`, `Response`, `Error` |
| `Stream Chat Completion (UnrealAI)` | Send an SSE request with incremental events | `Event`, `Completed`, `Failed`, `Cancelled`, `Async Action` |
| `Get First Choice Content` | Safely read the first assistant text | returned string, `Has Content` |
| `Make Json Object Response Format` | Request a JSON object | JSON string |
| `Make Strict Json Schema Response Format` | Request strict schema-constrained JSON | JSON string or empty string for invalid schema input |
| `Resolve Provider Config` | Resolve a settings profile | success boolean, provider config, error |
| `Reload Project Env File` | Reload local project-root `.env` values during editor testing | success boolean, variables loaded, status message |

`UnrealAIChatComponent` exposes `Send Prompt`, `Send Messages`, `On Chat Completed`, and `On Chat Failed`. Its streaming surface is `Send Prompt Stream`, `Send Messages Stream`, `Cancel Active Stream`, and the four `On Chat Stream ...` events.

## Messages and advanced fields

`UnrealAIChatMessage` supports `System`, `Developer`, `User`, `Assistant`, and `Tool` roles. Use `Content` for ordinary text.

`UnrealAIChatRequest.Stream` is deprecated. Keep it disabled and choose `Create Chat Completion (UnrealAI)` or `Stream Chat Completion (UnrealAI)` to make transport intent explicit.

Use advanced JSON pins only when needed. Their schema is selected-provider specific:

- `Content Json` replaces string content with a JSON value, including multimodal arrays.
- `Additional Fields Json` merges fields into one message object.
- `Response Format Json` sets the request's `response_format` object for OpenAI-compatible profiles.
- `Additional Parameters Json` merges fields into the request root.

Invalid JSON fails request construction. Prefer the provided response-format helper nodes over manually typed JSON for OpenAI-compatible profiles. Anthropic and Gemini support core text chat through the same nodes but do not yet normalize choice counts, tools, multimodal helpers, or structured-output helpers.

## Configuration and secrets

Provider profiles live under **Project Settings → Plugins → UnrealAI**. The built-in profiles use:

| Profile name | Default model | API key environment variable |
| --- | --- | --- |
| `OpenAI` | `gpt-5.6-luna` | `OPENAI_API_KEY` |
| `XAI` | `grok-4.6` | `XAI_API_KEY` |
| `Anthropic` | `claude-sonnet-5` | `ANTHROPIC_API_KEY` |
| `Gemini` | `gemini-3.7-flash` | `GEMINI_API_KEY` |

For local editor development, copy `.env.example` to `.env` in the consuming project root. Process environment variables take precedence. Never put a real key into a Blueprint asset, screenshot, source-controlled setting, or packaged client.

## Blueprint verification

- Compile the Blueprint after placing or reconnecting nodes.
- Confirm `Completed` handles a response with content and a response with no choices.
- Confirm `Failed` handles configuration errors and provider errors.
- For streaming graphs, confirm incremental text ordering, cancellation with a partial response, and all three terminal paths.
- Keep automated plugin tests offline; the repository tests cover Blueprint reflection, SSE framing, provider stream fixtures, request helpers, default profiles, and first-choice response handling without API keys.
- Exercise the native automation suite on each supported host through `Scripts/ci/run_unreal_ci.py` when changing the plugin itself.

The runtime graph is the same on macOS, Windows, and Linux. Only build and CI commands vary by host.
