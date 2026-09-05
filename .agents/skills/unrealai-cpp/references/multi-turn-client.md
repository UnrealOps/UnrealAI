# C++-Owned Multi-Turn Conversations

Use this reference when native code owns an ordered conversation across requests. One object must own the history, active request, commit policy, and cancellation state. Use `mixed-integration.md` only when Blueprint also participates.

For a multi-turn SSE conversation, load `streaming-conversation.md` instead; it combines the history transaction with delta, retry, partial-result, and terminal behavior.

## Native multi-turn recipe

The compile-tested implementation is `AUnrealAIMultiTurnExample` in:

- `Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIMultiTurnExample.h`
- `Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIMultiTurnExample.cpp`

Use the following state in the conversation owner:

- a retained `UPROPERTY` client;
- a `TArray<FUnrealAIChatMessage>` containing committed history;
- one active `FUnrealAIRequestHandle`;
- a request-in-flight flag; and
- a separate pending user message that is copied into the request but committed only with a successful assistant response.

### Start or reset a conversation

Reject reset while a request is active. Clear the array, then add the configured system message exactly once:

```cpp
ConversationHistory.Reset();
if (!SystemPrompt.TrimStartAndEnd().IsEmpty())
{
	ConversationHistory.Add(UUnrealAIBlueprintLibrary::MakeChatMessage(
		EUnrealAIMessageRole::System,
		SystemPrompt));
}
```

Do not also use `UUnrealAIChatComponent::SystemPrompt` when sending this array through `SendMessages`; messages functions intentionally send the supplied history unchanged.

### Submit one turn

Serialize requests within a conversation. Before the request:

1. reject empty input and an already-active turn;
2. prune only complete oldest user/assistant pairs while preserving the system prefix;
3. store one pending `User` message outside the committed array;
4. copy committed history followed by that pending message into `FUnrealAIChatRequest::Messages`; and
6. set the in-flight flag before calling `CreateChatCompletion`.

The client can invoke its completion delegate synchronously for configuration or request-building failures. Assign the returned handle only if the callback has not already cleared the in-flight flag:

```cpp
bRequestInFlight = true;
const FUnrealAIRequestHandle StartedRequest = UnrealAIClient->CreateChatCompletion(
	Request,
	FUnrealAIChatCompletionNativeDelegate::CreateUObject(
		this,
		&ThisClass::HandleTurnCompleted),
	FUnrealAIRetryNativeDelegate::CreateUObject(
		this,
		&ThisClass::HandleTurnRetry));

if (bRequestInFlight)
{
	ActiveRequest = StartedRequest;
}
```

### Commit or roll back

On a successful terminal callback, call `GetFirstChoiceContent`. Append the pending `User` and one `Assistant` message only when `bHasContent` is true; then clear the pending state. Do not append retry events or individual stream deltas to committed history.

On failure, cancellation, or an empty successful response, discard the pending user message and leave committed history unchanged. This sample policy lets the caller edit and resend the rejected user turn. A product may retain failed turns instead, but it must choose that policy explicitly and prevent a second commit of the same turn.

Keep history bounded by removing complete oldest user/assistant pairs. Preserve the leading system message and never truncate halfway through a pair. Summarization can replace removed pairs, but it is application behavior rather than an UnrealAI SDK feature.

## Verification

- Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` on a matching Unreal host. The runner builds the sample editor target, so UnrealHeaderTool and the native compiler validate the example.
- `UnrealAISample.SkillContracts.Cpp.MultiTurnBehavior` executes reset, successful commit, failure rollback, role ordering, and bounded pair trimming without provider credentials.
- Extend the behavioral contract when the recipe's commit, failure, cancellation, retry, or history-bound policy changes. Do not replace those checks with source-text assertions.
