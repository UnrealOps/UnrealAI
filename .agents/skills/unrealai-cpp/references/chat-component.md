# C++ Actor Component Integration

Use this reference when an Actor owns `UUnrealAIChatComponent`. Use `one-shot-client.md` or `streaming-client.md` instead when the caller needs per-request native handles, custom delegate captures, or direct client ownership.

## Configure and bind

Create the component as a default subobject or otherwise keep it owned by the Actor. Configure `ProviderName`, optional `Model`, `SystemPrompt`, temperature, and `RetryOptions` before sending.

For one-shot calls, bind `OnChatCompleted`, `OnChatRetrying`, `OnChatFailed`, and `OnChatCancelled`, then call `SendPrompt` or `SendMessages`. `CancelActiveCompletions` cancels every active one-shot request owned by the component.

For streaming, bind `OnChatStreamRetrying`, `OnChatStreamEvent`, `OnChatStreamCompleted`, `OnChatStreamFailed`, and `OnChatStreamCancelled`, then call `SendPromptStream` or `SendMessagesStream`. `CancelActiveStream` cancels the current stream. A component owns at most one active stream; a second start reports `stream_already_active` without replacing the existing request.

The component owns and retains its internal client. Do not create a second client for the same flow.

## History and system prompt

The component is stateless request plumbing:

- `SendPrompt` creates a fresh array containing the current `SystemPrompt` when non-empty, followed by the new user prompt.
- `SendPromptStream` creates the same fresh system-plus-user array.
- `SendMessages` and `SendMessagesStream` send the supplied array exactly and do not prepend `SystemPrompt`.

For multi-turn chat, keep a `TArray<FUnrealAIChatMessage>` in one application-owned conversation object. Add the system message once, append one user message before sending, and append one assistant message only after a successful terminal response. Choose and test whether failure or cancellation rolls back the pending user turn. Use [multi-turn-client.md](multi-turn-client.md) for the complete state machine.

## One-shot concurrency

The component accepts simultaneous one-shot calls, but `OnChatCompleted`, `OnChatFailed`, and `OnChatCancelled` do not expose a request handle. `OnChatRetrying` carries the logical handle, but a later terminal event still cannot be matched to it. Completion order is not guaranteed.

When the result must map to a prompt:

- serialize component calls and start the next after a terminal event;
- use a separate component or async action for each independent owner; or
- use `UUnrealAIClient` directly and capture an application-owned request ID in its completion delegate.

Do not infer correlation from submission order. `CancelActiveCompletions` is an all-active cancellation operation, not cancellation by handle.

## Verification

`UnrealAI.ChatComponent.RequestConstruction` verifies that prompt functions construct system-plus-user messages while messages functions preserve caller-owned history without injection. Plugin reflection tests protect the documented component functions and delegates. Run `Scripts/ci/run_unreal_ci.py --platform <Mac|Win64|Linux>` on a matching host after changing this integration.
