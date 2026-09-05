# Packaged Blueprint Streaming Conversation

Use this combined recipe when a packaged game's Widget Blueprint presents a multi-turn stream while an authenticated external backend owns provider credentials, the system prompt, model policy, moderation, and budgets. It is the canonical cross-boundary recipe; do not also load `async-actions.md`, `multi-turn-component.md`, `mixed-integration.md`, `packaged-client-deployment.md`, or `backend-deployment.md` unless changing an underlying contract.

The compile-tested implementation is split deliberately:

- `UUnrealAIPackagedClientFacadeExample` owns the authenticated backend transport and exposes no reflected credential, URL, provider, model, or header input. `Stream Turn` accepts reflected `FUnrealAIChatMessage` values, but the validated boundary rejects their `ContentJson`, `AdditionalFieldsJson`, name, and tool-call fields at runtime.
- `UUnrealAIPackagedChatWidgetExample` owns transactional conversation state and exposes presentation-only Blueprint events.
- generated `WBP_UnrealAIBackendChat` subclasses that widget controller and contains all six presentation-event entry points. It is an intentionally layout-free, compiled skeleton; the consuming game supplies its widget tree, styling, localization, and connections from those events to visual updates.

Keeping the transaction owner native prevents a graph from accidentally committing partial text, accepting stale request IDs, or exposing a session token while still leaving layout and presentation in Blueprint.

## Native acquisition and token refresh

The owning game-instance subsystem or player controller must retain both native objects with `UPROPERTY`. It obtains a short-lived game-session token, calls the facade's non-reflected `Initialize`, creates the widget, and calls its non-reflected `AttachInitializedFacade`. The attach call enforces that the facade is initialized and idle. Blueprint never receives the token.

Native session code binds the facade's native `OnFailed` delegate before handing the facade to the widget. On the public `session_expired` reason (HTTP 401), the controller immediately sets `Backend Ready` false and broadcasts `Receive Backend Readiness Changed(false)`, so presentation disables Retry. Native session code schedules refresh work for a later game-thread tick; after the widget has processed the terminal broadcast, it verifies `Active Request Id` is invalid, obtains a new token, initializes a replacement facade, and calls `AttachInitializedFacade`. Successful attachment preserves the failed/retryable turn state, sets `Backend Ready` true, and broadcasts `Receive Backend Readiness Changed(true)`; only then may presentation enable the player's explicit Retry action. The controller rejects a raced Retry with `backend_refreshing` while preserving its retry text. It never silently replays a partially displayed turn. `access_denied` (HTTP 403) is a non-retryable authorization failure and must neither refresh the token nor enable Retry.

Do not put `Stream Chat Completion (UnrealAI)` directly in this packaged widget. That public SDK node is appropriate for trusted processes or local tools, but it intentionally exposes provider-profile selection and does not acquire private game-session tokens.

## Blueprint surface

Call these inherited commands from the generated widget:

- `Submit Turn(User Text, Out Failure Reason)`
- `Retry Last Turn(Out Failure Reason)`
- `Cancel Turn`

Read `Committed History`, `Request State`, `Active Request Id`, `Pending Assistant Text`, `Interrupted Assistant Text`, `Backend Ready`, and the allow-listed `Last Public Failure Reason`. Implement these six generated events:

- `Receive Backend Readiness Changed(Is Ready)`
- `Receive Text Delta(Text Delta)`
- `Receive Retrying(Retry Number, Max Retries, Delay Seconds)`
- `Receive Completed(Final Text)`
- `Receive Failed(Public Reason, Partial Text)`
- `Receive Cancelled(Partial Text)`

The Blueprint owns only a bounded display transcript and localized UI state. It must not keep a second provider-history array. `Committed History` always contains complete `User, Assistant` pairs, has no system message, and is not mutated by text deltas, automatic retry, failure, or cancellation. The controller prunes a copy for the outbound request, then applies committed-history eviction only after a successful pair is committed. For the sample presentation, use one metric consistently: cap the display transcript at 32 rows, 2,048 `FString::Len()` units per row, and 32,768 such units across all stored transcript text. Evict oldest finalized rows as whole user/assistant turn units and never evict or split the active temporary rows. Before inserting a new provisional pair, first require the canonical user value to be nonempty and at most 2,048 `Len()` units. Compute an eviction plan against a copy of the row view-model array until at most 30 rows would remain and `CurrentLength + CanonicalUserLength + 2,048 <= 32,768`; do not remove a widget while testing feasibility. Reserve that full assistant allowance while deltas arrive, so the hard aggregate cap also holds during an active turn. If the canonical input fails its row bound or no plan can fit after all eligible evictions, create no provisional rows, mutate no transcript state, and surface the allow-listed local failure. Immediately before applying a legal plan, snapshot every victim's complete presentation view model and original index plus the prior active row references/resolved flag into transient rollback members. Then remove the planned whole turn units and insert/reuse the provisional rows. Restore that snapshot if native submission rejects without a synchronous terminal; discard it only after acceptance or a synchronous terminal. On Retry, exclude the reused interrupted assistant text from the calculation, reserve its full 2,048-unit replacement allowance, and plan eviction of only other finalized turn units. Render localized status labels outside stored transcript text, or give them a separate explicit bound. These display limits match the compiled sample's `FString::Len()` checks; they are not UTF-8 byte or token claims, and the backend applies those separate limits.

Recommended widget state mapping:

```text
Unavailable -> Idle -> Starting -> Streaming
                         |            |
                         +-> Retrying-+
Starting/Streaming/Retrying -> Cancelling -> Cancelled
Starting/Streaming/Retrying -> Failed
Starting/Streaming/Retrying -> Idle on completion
```

## Exact graph behavior

On Submit:

1. Copy the edit-box value into an immutable raw-attempt member. In Blueprint, apply the `Trim` string node once to remove leading whitespace, then apply `Trim Trailing` once to that result; store the second result as the immutable canonical attempt. (`Trim` alone is not equivalent to native `TrimStartAndEnd()`.) Before eviction or provisional-row creation, reject an empty canonical value or one whose `Len()` exceeds 2,048 as `invalid_message_content`; keep the edit box and transcript unchanged and do not call `Submit Turn`. This exactly mirrors the controller's compiled example preflight. Use that same stored canonical value for the row, length accounting, and `Submit Turn`; never rebuild it from the raw edit-box value.
2. Compute the pre-insertion display eviction/reservation plan without changing the live row array. If no legal plan exists, keep the edit box and transcript unchanged, show the application's allow-listed display-limit reason, and do not call `Submit Turn`. For a legal plan, snapshot the planned victim view models/indices and the prior active row references/resolved flag into transient **Widget Blueprint member variables**, apply the planned evictions, then create one provisional user row from the immutable canonical value and one empty provisional assistant row. Store both new row references plus `bProvisionalTurnResolved` as transient members, set the flag false, and leave the edit-box value intact. These cannot be function locals because asynchronous lifecycle event entry points must read and update them. The rows must exist before submission because a terminal event can be delivered synchronously inside the call.
3. Call `Submit Turn` exactly once with the immutable trimmed attempt value.
4. Every terminal event sets `bProvisionalTurnResolved = true` after finalizing those provisional rows.
5. If the call returns false and the flag is still false, no controller transaction was prepared: remove both provisional rows, reinsert every snapshotted victim at its original index, restore the prior active row references/resolved flag, leave the text editable, and display the localized `Out Failure Reason` once. If the flag is true, a synchronous terminal already presented the result; discard the rollback snapshot and do not add another error or remove its interrupted row.
6. If the call returns true, discard the rollback snapshot, keep the same rows, and clear the edit box. Re-read `Request State`: a synchronous terminal may already have resolved the turn, so enable Cancel only while the controller remains active. Do not append provider history in Blueprint.

On the generated events:

- `Receive Text Delta`: append only to the active temporary assistant row. The controller already rejected stale IDs and bounded the aggregate.
- `Receive Retrying`: show generic progress from the ordinal/count/delay; do not resubmit or mutate transcript/history.
- `Receive Completed`: replace/finalize that same temporary row with `Final Text`, set the provisional-resolved flag, and do not add a second assistant row. Native state already committed exactly one provider-history pair. Apply the display-row bounds after finalization.
- `Receive Failed` and `Receive Cancelled`: replace/finalize that same temporary row with `Partial Text`, mark it interrupted, and set the provisional-resolved flag. Never copy it into provider context. If it is empty, retain a status-only interrupted row. Apply the display-row bounds. Enable explicit Retry for ordinary retryable failure/cancellation only when `Backend Ready` is true; `session_expired` waits for `Receive Backend Readiness Changed(true)`, and `access_denied` stays non-retryable.
- The Retry button computes its eviction plan on a copy, then snapshots the interrupted assistant row's text/style, every planned eviction victim and original index, and the prior active row references/resolved flag into transient rollback members. Only after the plan fits does it apply evictions, reuse the interrupted turn's user row, clear/reuse its assistant row, point the active row-reference members at that pair, set `bProvisionalTurnResolved = false`, and call `Retry Last Turn` exactly once. The controller creates a new GUID and resubmits the saved canonical user text; never append a duplicate user row for the same visible turn. Apply the same synchronous reconciliation as Submit: every terminal event sets the flag after finalizing the reused rows; after return, a false result with a false flag restores the assistant text/style, every evicted victim in order, and all prior active members, displays `Out Failure Reason` once, and retains native retry text; a false result with a true flag means a synchronous terminal already presented the attempt and the whole snapshot is discarded; and a true result also discards the snapshot, rereads `Request State`, and enables Cancel only if still active. If planning fails, mutate nothing and do not call Retry. This covers synchronous terminal delivery and local retry rejection without blanking the old partial, losing finalized turns, or duplicating status/error rows.
- The Cancel button calls `Cancel Turn`. When it returns true, wait for `Receive Cancelled`; when false, reread `Request State` instead of clearing a possibly newer request. The native controller restores the old state only if the same request ID remains active, so a synchronous terminal during cancellation wins the race.
- On widget destruct, the controller stops accepting events, unbinds all five facade delegates, cancels the active request, and releases the facade.

The compiled limits are 16 committed messages, 2,048 `FString::Len()` units per message/output, and 8,192 such units of provider history. Oldest complete pairs are removed first. Final text is trimmed and must be nonempty; therefore every successful assistant message is valid when sent on the next turn. Boundary truncation never retains half of a UTF-16 surrogate pair. Treat these length limits as an application example—production services should additionally apply UTF-8 byte/token limits.

## Idempotency and backend contract

For every logical turn, the facade adds this stable compatible-payload extension:

```json
{"game_request_id":"<lowercase-hyphenated-guid>"}
```

The SDK reuses the same request object across pre-first-event transport retries, so the value is unchanged. The backend must authenticate the session, bind that ID to the player/conversation, collapse in-flight duplicates, return the same terminal result for a completed duplicate according to its retention policy, and strip `game_request_id` before forwarding provider-native JSON. Automatic streaming retry is safe only after that contract exists. The SDK itself never replays a stream after a complete SSE data event.

The backend must also independently validate UTF-8 byte/token bounds, authorize the conversation, choose the provider/model/system prompt, moderate input/output, enforce quotas and circuit breakers, propagate disconnect cancellation upstream, and return OpenAI-compatible text SSE. Public failure reasons are an allow-list; raw provider errors, IDs, bodies, and headers never cross the facade. Define the deployed endpoint/model alias, request-ID retention/replay format, localization table, and load/security thresholds in the consuming application's backend contract.

## Generated asset and validation

Load `asset-workflow.md` when changing generation. `UnrealAISampleGenerate` creates a real `UWidgetBlueprint` with `UWidgetBlueprintGeneratedClass`, emits the six exact override-event entry points using reflected function names, compiles before saving, and the contract runner reloads it from an isolated sample copy. The asset is generated only in that isolated copy; it is not a checked-in, pre-styled widget and must not be presented as one.

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` on a matching host. The required contracts prove:

- `Blueprint.BackendFacade`: exact delegate signatures, explicit absence of credential/configuration properties, runtime rejection of every reflected advanced message field, role/field bounds, canonical messages, stable transmitted request ID, normalized-event filtering, distinct authentication/authorization mapping, and output bounds including BMP/astral exact-boundary behavior without split surrogate pairs. The core adapter contract proves the extension reaches serialized wire JSON, and the retry-coordinator contract proves the originally serialized body is reused;
- `Blueprint.GeneratedBackendWidget`: the real Widget Blueprint skeleton reloads, compiles, inherits the native controller, contains all six event entry points, and has an empty widget tree/root; and
- `Blueprint.PackagedWidgetBehavior`: facade-to-widget broadcasts, stale-ID rejection across every lifecycle event, retry invariance, actual retry preservation on local rejection, exact-once completion, canonical staged history and full-limit rollback, failure/cancellation rollback, cancel-false and synchronous-terminal reconciliation, output-limit reasons, display-only partials, whitespace rejection, initialized facade replacement, teardown unbinding, and concurrency rejection.

The runner executes and report-checks both the `UnrealAISample.SkillContracts` sample group and the core `UnrealAI.` plugin group declared in `Scripts/ci/skill_contracts.json`; missing/skipped/incomplete core wire or retry tests fail the run. These contracts validate the reusable native controller and the structure of the layout-free skeleton. They do **not** prove provisional-row graph behavior, exact-once visual presentation, retry row reuse/rollback, edit-box behavior, localization, the active 32-row/32,768-`Len()`-unit display policy, application-specific layout, real token refresh, or a deployed backend. Those remain consuming-game responsibilities and require project-level Widget Blueprint/PIE tests after the presentation graph is implemented, including leading-plus-trailing canonicalization with the exact `Trim` then `Trim Trailing` node sequence, oversized canonical-input rejection before row insertion, rollback of every evicted row and prior active reference on rejected Submit/Retry, a rejected Retry that preserves the prior visible partial, and cap checks while a provisional turn is active.
