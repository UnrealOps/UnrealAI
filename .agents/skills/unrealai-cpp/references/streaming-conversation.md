# C++ Multi-Turn Streaming Conversation

Use this combined reference when one native object owns both conversation history and an SSE stream. Do not also load `streaming-client.md` or `multi-turn-client.md` unless changing their lower-level transport or one-shot details.

The compile-tested implementation is `AUnrealAIMultiTurnExample` under `Samples/UnrealAISample`. It exposes `SendTurnStream`, `CancelTurn`, normalized `OnTurnTextDelta`, and the same completed, retrying, failed, and cancelled lifecycle events as its one-shot path.

## State and ownership

The retained conversation owner keeps:

- a `UPROPERTY` `UUnrealAIClient`;
- committed `ConversationHistory` with at most one leading system message;
- one active logical request handle and request-in-flight flag;
- a separate pending user message that is copied into the request but is not visible in committed history;
- a streaming-mode flag; and
- `PendingAssistantText` plus display-only `LastInterruptedAssistantText`.

Serialize turns within one conversation. If several players or conversations are active, create one owner per conversation and enforce application-level per-player and fleet admission limits above them.

## Transactional turn recipe

1. Reject empty input and a second active turn.
2. Configure the retained client, prune only complete oldest user/assistant pairs, and preserve the system prefix.
3. Keep the pending user message separate. Copy committed history and then that pending message into `FUnrealAIChatRequest::Messages`; do not expose an array named committed history that temporarily contains uncommitted state.
4. Set the in-flight state before calling `StreamChatCompletion`, because a configuration or request-build failure may invoke the terminal delegate synchronously.
5. Retain the returned handle only if the terminal callback has not already cleared the state.
6. On `TextDelta`, append only normalized text to the temporary display buffer. Ignore `ProviderEvent` unless an explicitly provider-specific feature owns and secures that data.
7. Treat retry as progress for the same logical operation. Do not mutate history or clear the pending snapshot.
8. On `Completed`, prefer `GetFirstChoiceContent` from the aggregate response. Commit the pending user and exactly one assistant message only when content exists, then clear the pending state.
9. On `Failed`, `Cancelled`, or empty completion, discard the pending user. Preserve partial text only in explicitly interrupted display state; never send it as a committed assistant turn by accident.
10. Ignore stale or duplicate terminal work after the owner has cleared its in-flight flag.

The SDK may retry a stream only before its first complete SSE data event. Do not layer an automatic application replay over a partially delivered stream. A user-selected retry after a terminal state is a new logical turn attempt and must append its pending user message exactly once.

Cancel the retained handle when the user cancels, an application deadline expires, the owner disconnects, the world travels, or the result loses relevance. Explicit cancellation produces a cancelled terminal; destruction cleanup can suppress delegates, so perform product-visible cancellation before destroying the owner when the UI requires a terminal notification.

## Blueprint presentation

Blueprint may bind to the five local presentation routes exposed by the sample owner:

- `On Turn Text Delta`
- `On Turn Retrying`
- `On Turn Completed`
- `On Turn Failed`
- `On Turn Cancelled`

Only `On Turn Text Delta` is narrowed to normalized text. `On Turn Failed` and `On Turn Retrying` deliberately expose full SDK structs for a same-process trusted UI and can contain raw provider data, HTTP status, or the SDK request handle. Do not replicate those structs or call these five routes sanitized. For a remote player, translate them in authoritative server code into ordered, bounded application DTOs with allow-listed public failure codes and retry fields.

## Verification

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` on a matching host. `UnrealAISample.SkillContracts.Cpp.StreamingConversationBehavior` exercises normalized delta accumulation, provider-event exclusion, retry invariance, aggregate commit, duplicate-terminal rejection, failure/cancellation discard, partial display state, and concurrent-turn rejection without credentials. `Mixed.BlueprintSurface` protects the five reflected local presentation routes.
