# Blueprint Actor Component Integration

Use this reference when an Actor Blueprint owns `UnrealAIChatComponent`. Use `async-actions.md` when each request should have its own graph-local async action and terminal routes.

## Configure and bind

1. Add an `UnrealAIChatComponent` to the Actor Blueprint.
2. Set `Provider Name`, optional `Model`, optional `System Prompt`, optional temperature, and `Retry Options` in Details.
3. Bind component delegates once during initialization rather than before every send.
4. For one-shot work, bind `On Chat Completed`, `On Chat Retrying`, `On Chat Failed`, and `On Chat Cancelled`, then call `Send Prompt` or `Send Messages`.
5. For streaming, bind `On Chat Stream Retrying`, `On Chat Stream Event`, `On Chat Stream Completed`, `On Chat Stream Failed`, and `On Chat Stream Cancelled`, then call `Send Prompt Stream` or `Send Messages Stream`.
6. Use `Get First Choice Content` and `Has Content` on complete or partial responses. Read `UnrealAIError.Message` on failure without exposing raw JSON.

The component creates and retains its internal client. Do not create a second client for the same flow.

## Cancellation and stream limit

`Cancel Active Completions` cancels all active component one-shot requests. `Cancel Active Stream` cancels the current stream. A component owns at most one active stream; starting a second reports `stream_already_active` without cancelling or replacing the existing stream.

Retry events are non-terminal progress notifications. Cancellation remains available while the request waits in backoff. Treat `Cancelled` separately from `Failed`, and preserve any partial stream response needed by the UI.

## History and system prompt

The component does not retain conversation history:

- `Send Prompt` creates a fresh array containing `System Prompt` when non-empty, followed by the user prompt.
- `Send Prompt Stream` creates the same fresh system-plus-user array.
- `Send Messages` and `Send Messages Stream` send the supplied array exactly and do not inject `System Prompt`.

For multi-turn chat, keep one `UnrealAIChatMessage` array in the owning Blueprint. Add a system message once, append the user before sending, and append the assistant only after successful content extraction. Do not combine a prepared system message with `Send Prompt` and expect deduplication. Use [multi-turn-component.md](multi-turn-component.md) for commit, rollback, and bounded-history behavior.

## Concurrent one-shot requests

The component accepts multiple simultaneous one-shot calls, but its terminal delegates expose only response and error. They do not expose a request handle, and completion order is not guaranteed. A retry event's handle cannot make the later terminal event identifiable.

When a graph must map each result to its prompt:

- gate submission with a `Request In Flight` Boolean and clear it on every terminal event;
- use a separate component for each independent owner; or
- use a separate `Create Chat Completion (UnrealAI)` async action for each operation.

Do not correlate by submission order. `Cancel Active Completions` cannot select one request.

## Verification

Compile the owning Blueprint and exercise every bound terminal route. `UnrealAI.ChatComponent.RequestConstruction` protects the system-prompt and caller-owned message behavior, while `UnrealAI.Blueprint.Surface` protects the reflected delegates and functions. Run `Scripts/ci/run_unreal_ci.py --platform <Mac|Win64|Linux>` after changing this contract.
