# Blueprint Async Actions

Use this reference for `Create Chat Completion (UnrealAI)` and `Stream Chat Completion (UnrealAI)`. Read `requests-and-providers.md` only when constructing multi-message or advanced requests or changing provider configuration. Use `chat-component.md` for component-owned calls.

Do not use the provider-facing async action directly in a packaged client that must authenticate to an external game backend. Use `backend-streaming-conversation.md` for that combined facade and history recipe.

## One-shot async flow

Use this pattern for one request:

1. Add `Make Simple Chat Request` and enter the prompt. Leave `Model` empty to use the provider default.
2. Add `Create Chat Completion (UnrealAI)` and connect the request value.
3. Set `Provider Name` to `OpenAI`, `XAI`, `Anthropic`, `Gemini`, a custom profile name, or `None` for the default profile.
4. Connect the async node's `Completed` execution path and `Response` value to `Get First Choice Content`.
5. Branch on `Has Content` before consuming the returned string.
6. Connect `Failed`, break the supplied `UnrealAIError`, and display or handle `Message` without exposing secrets.
7. Use `Retrying` for progress or telemetry and `Cancelled` for explicit cancellation. Promote `Async Action` to a variable before calling its inherited `Cancel` function.

The README illustration at `Documentation/Images/blueprint-chat-completion.png` shows the compact success path. It is an illustration, not a literal editor capture; add the failure branch in production graphs.

## Streaming async flow

Use this pattern when the graph needs incremental text or cancellation:

1. Add `Make Simple Chat Request`, leave its deprecated `Stream` field disabled, and connect it to `Stream Chat Completion (UnrealAI)`.
2. Select `Provider Name` exactly as for the one-shot node.
3. Promote the exposed `Async Action` output to a variable when the graph needs to call `Cancel` later.
4. On `Event`, switch on `Event.Type`. Append `Event.Text Delta` only for `Text Delta`; use `Choice Finished` and `Usage` when their normalized values matter.
5. Handle `Completed` as the full aggregate response and `Failed` as an error plus any partial response.
6. Handle `Cancelled` separately. Its `Partial Response` contains any normalized text received before cancellation.
7. Treat `Retrying` as a non-terminal notification. UnrealAI retries a stream only before its first complete SSE data event.

`Provider Event` exposes native non-text or otherwise unnormalized JSON. Treat it as provider-specific, potentially sensitive data and do not print it by default. The node emits stream events in order and exactly one terminal path.

## Node catalog

| Blueprint display name | Purpose | Important outputs |
| --- | --- | --- |
| `Make Chat Message` | Create a role and content pair | `UnrealAIChatMessage` |
| `Make Simple Chat Request` | Create a one-user-message request | `UnrealAIChatRequest` |
| `Create Chat Completion (UnrealAI)` | Send an asynchronous request | `Completed`, `Retrying`, `Failed`, `Cancelled`, `Async Action` |
| `Stream Chat Completion (UnrealAI)` | Send an SSE request with incremental events | `Event`, `Retrying`, `Completed`, `Failed`, `Cancelled`, `Async Action` |
| `Get First Choice Content` | Safely read the first assistant text | returned string, `Has Content` |
| `Make Json Object Response Format` | Request a JSON object | JSON string |
| `Make Strict Json Schema Response Format` | Request strict schema-constrained JSON | JSON string or empty string for invalid schema input |
| `Resolve Provider Config` | Resolve a settings profile | success boolean, provider config, error |
| `Reload Project Env File` | Reload local project-root `.env` values during editor testing | success boolean, variables loaded, status message |

## Retry controls

Every provider profile defaults to two retries, a one-second initial delay, exponential backoff with up to 25 percent jitter, and a 60-second delay ceiling. Expand `Retry Options` on a request or component to select:

- `Use Provider Policy` to inherit the provider profile.
- `Disabled` to make only the initial attempt.
- `Override Maximum Retries` to change the retry count while retaining the provider timing policy.

`Retrying` exposes a `Retry Event` with `Request Handle`, `Retry Number`, `Max Retries`, `Delay Seconds`, `Reason`, and `Http Status`. It fires before the delay and does not replace the eventual terminal path. Calling the async action's inherited `Cancel` during backoff is supported.

## Blueprint verification

- Compile the Blueprint after placing or reconnecting nodes.
- Confirm `Completed` handles a response with content and a response with no choices.
- Confirm `Failed` handles configuration errors and provider errors.
- Confirm `Cancelled` is distinct from failure for one-shot and streaming graphs.
- Confirm retry notifications do not trigger terminal logic and cancellation works during backoff.
- For streaming graphs, confirm incremental text ordering, no replay after partial output, cancellation with a partial response, and all three terminal paths.
- Keep automated plugin tests offline; the repository tests cover Blueprint reflection, SSE framing, provider stream fixtures, request helpers, default profiles, and first-choice response handling without API keys.
- Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` for sample compilation and Blueprint asset contracts. Exercise the full native suite through `Scripts/ci/run_unreal_ci.py` when changing the plugin itself.

The runtime graph is the same on macOS, Windows, and Linux. Only build and CI commands vary by host.
