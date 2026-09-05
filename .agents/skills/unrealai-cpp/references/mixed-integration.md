# Mixed C++ and Blueprint Integration

Use this reference when native code and Blueprint share one UnrealAI feature. Choose one owner for conversation history and request state before implementing either side.

## C++ owns the conversation; Blueprint owns presentation

Use this boundary for an authoritative server, shared gameplay service, persistence, or a Blueprint UI that should not manipulate credentials or provider state.

The compile-tested `AUnrealAIMultiTurnExample` exposes:

- `ResetConversation`, `SendTurn`, `SendTurnStream`, and `CancelTurn` as `BlueprintCallable` commands;
- `ConversationHistory`, `bRequestInFlight`, and temporary/interrupted streaming text as `BlueprintReadOnly` state; and
- `OnTurnTextDelta`, `OnTurnCompleted`, `OnTurnFailed`, `OnTurnCancelled`, and `OnTurnRetrying` as `BlueprintAssignable` events.

The Blueprint subclass or Widget binds each event once, resets at the session boundary, disables submission while a request is active, and renders event payloads. It must not append to `ConversationHistory`; C++ already commits or rolls back each turn. When `SendTurn` returns false, handle its `Out Error` as a local rejection rather than a terminal request event.

For a dedicated server, invoke `SendTurn` or `SendTurnStream` only in authoritative server code. Delegates do not cross the network: send bounded prompts through the game's validated RPC layer and return ordered, authorized, sanitized response data. Load `streaming-conversation.md` for the combined transaction and `dedicated-server-deployment.md` for the module/RPC boundary.

## Blueprint owns the request; C++ supplies helpers

Use async Blueprint nodes when designers need to own the flow graph. Native code may provide domain-specific `BlueprintCallable` helpers or terminal sinks, as the generated `BP_UnrealAIGettingStarted` does with `AUnrealAISampleActor`.

- Blueprint owns and updates the history array.
- Native helpers do not keep a second hidden history.
- The async node's terminal pins define the turn lifetime.
- Pass sanitized domain values into C++ rather than raw provider JSON when possible.

Do not combine both ownership patterns in one instance. Two writers cause duplicated turns and ambiguous rollback.

## Verification

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` on a matching host. `UnrealAISample.SkillContracts.Mixed.BlueprintSurface` verifies the callable one-shot/streaming commands, read-only state, and five assignable streaming lifecycle events. `Cpp.StreamingConversationBehavior` protects the combined history policy.
