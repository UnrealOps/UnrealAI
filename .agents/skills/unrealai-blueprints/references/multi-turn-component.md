# Blueprint-Owned Multi-Turn Conversations

Use this reference when Blueprint owns conversation history across calls to `UnrealAIChatComponent`. Choose one owner for history and request state before placing nodes. Use `mixed-integration.md` when C++ also participates.

For a packaged client whose external backend owns provider credentials, use `backend-streaming-conversation.md` instead of this direct component recipe.

## Blueprint-owned multi-turn component recipe

This recipe uses `UnrealAIChatComponent` for transport and an owning Actor, Widget, or conversation object for history.

Create these variables:

| Variable | Type | Initial value |
| --- | --- | --- |
| `History` | Array of `UnrealAIChatMessage` | empty |
| `Request In Flight` | Boolean | false |
| `Pending History Length` | Integer | `-1` |
| `Pending Assistant Text` | String | empty; needed only for streaming |

Bind every component event once during initialization, not before every submission. Leave the component's `System Prompt` empty because this recipe sends an explicit message array.

### Initialize

1. Clear `History` and set `Pending History Length` to `-1`.
2. If the system prompt is non-empty, call `Make Chat Message` with role `System` and add the result to `History` once.
3. Do not add another system message per turn.

### Submit a one-shot turn

1. Reject the submit action if `Request In Flight` is true or the trimmed input is empty.
2. Before appending, prune only complete oldest user/assistant pairs and preserve element zero when it is the system message.
3. Set `Pending History Length` to the current `History` length.
4. Call `Make Chat Message` with role `User`, add it to `History`, and set `Request In Flight` true.
5. Call the component's `Send Messages` with the complete `History` array. Do not call `Send Prompt`; it would construct a new stateless message list.

Handle the bound events as follows:

- `On Chat Retrying`: show retry progress only. Do not change `History`, `Pending History Length`, or `Request In Flight`.
- `On Chat Completed`: call `Get First Choice Content`. When `Has Content` is true, add one `Assistant` message, then set `Pending History Length` to `-1` and `Request In Flight` false. When it is false, apply the failure rollback below.
- `On Chat Failed`: resize `History` to `Pending History Length`, then reset the pending length and in-flight flag. Present `UnrealAIError.Message` without logging raw JSON.
- `On Chat Cancelled`: apply the same rollback and clear the in-flight flag, but present cancellation separately from failure.

This rollback policy removes a user turn that did not receive an assistant response. Keeping it is also valid if the product requires an audit trail, but choose one policy and avoid appending the same turn twice on resubmission.

## Blueprint-owned streaming multi-turn recipe

Use the same variables and submission sequence, but call `Send Messages Stream`.

- Clear `Pending Assistant Text` before submitting.
- On `On Chat Stream Event`, append only `Text Delta` values to `Pending Assistant Text` for display. Never append each delta as a history message.
- On `On Chat Stream Retrying`, leave history unchanged.
- On `On Chat Stream Completed`, read the aggregate response with `Get First Choice Content` and append exactly one `Assistant` message. Prefer the aggregate over the delta buffer because it is the SDK's normalized final response.
- On `On Chat Stream Failed`, roll back to `Pending History Length`. A partial response is available for UI or diagnostics but should not become committed history accidentally.
- On `On Chat Stream Cancelled`, decide whether the partial assistant response is meaningful. The conservative policy is to roll back the pending user turn and keep partial text display-only. If the game commits partial output, append it once and label that turn as interrupted in application state.
- Clear `Request In Flight`, pending length, and pending text on every terminal event.

The component permits only one active stream, which matches this serialized conversation recipe.

## Verification

- Compile the Blueprint and confirm all four one-shot or five streaming event bindings resolve.
- Run at least two serialized turns and inspect role order: one optional `System`, followed by alternating `User` and `Assistant` messages.
- Force failure and cancellation and confirm the selected rollback policy, `Request In Flight`, and pending variables all reset.
- For streaming, confirm deltas update only the temporary display buffer and one aggregate assistant message is committed.
- Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` on a matching host. `UnrealAI.ChatComponent.RequestConstruction` protects messages-array and system-prompt semantics; `Cpp.MultiTurnBehavior` exercises the equivalent commit, rollback, and history-bound policy in the compile-tested native owner.
