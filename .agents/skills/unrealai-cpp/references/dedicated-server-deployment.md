# Dedicated-Server Deployment

Use this reference only for a controlled dedicated-server process. A listen server is player-controlled and must follow `packaged-client-deployment.md`. Use `backend-deployment.md` when provider policy belongs in a separate service. Load `streaming-conversation.md` as well when the server owns multi-turn streaming state.

UnrealAI supplies provider transport, normalization, retry, cancellation, and game-thread callbacks. The consuming game must supply the network protocol, identity, authorization, abuse/cost controls, output policy, persistence, reconnect behavior, and build-target boundary described below.

## Server-only build boundary

Use three application modules:

```text
GameConversationProtocol   shared bounded DTOs/RPC endpoint; no UnrealAI dependency
GameConversationServer     provider client/history/policy; private UnrealAI dependency
Game                       client UI/presentation; no provider types or credentials
```

In the consuming `.uproject`, keep the server module target-scoped and do not globally enable UnrealAI when only the server needs it:

```json
{
  "Name": "GameConversationServer",
  "Type": "Runtime",
  "LoadingPhase": "Default",
  "TargetAllowList": ["Server"]
}
```

```csharp
// Game.Target.cs
Type = TargetType.Game;
ExtraModuleNames.AddRange(new[] { "Game", "GameConversationProtocol" });
DisablePlugins.Add("UnrealAI");

// GameServer.Target.cs
Type = TargetType.Server;
ExtraModuleNames.AddRange(new[]
{
    "Game", "GameConversationProtocol", "GameConversationServer"
});
EnablePlugins.Add("UnrealAI");
```

`GameConversationServer.Build.cs` uses `PrivateDependencyModuleNames.Add("UnrealAI")`. Its public headers must not expose UnrealAI types. Neither the Game target nor `GameConversationProtocol` may depend on UnrealAI. If any other Game module uses the plugin directly, the binary cannot be claimed credential-isolated; move that feature behind the server/backend boundary first.

## Shared protocol

Attach the RPC component to the remote player's owned controller or pawn. A Server RPC on an unowned world actor is not a valid client-to-server channel.

Use application DTOs, not `FUnrealAIError`, `FUnrealAIRetryEvent`, request handles, provider config, headers, raw JSON, usage/provider IDs, or full history:

```cpp
UENUM(BlueprintType)
enum class EGameConversationEventType : uint8
{
    Started, TextReset, TextDelta, Retrying, Completed, Failed, Cancelled
};

UENUM(BlueprintType)
enum class EGameConversationPublicCode : uint8
{
    None, InvalidRequest, RequestNotStarted, Unauthorized, Busy, RateLimited,
    ModerationRejected, Timeout, OutputLimit, ServiceUnavailable,
    EmptyResponse, ClientBackpressure, OperationLimit,
    TurnExpired, ConversationExpired
};

UENUM(BlueprintType)
enum class EGameConversationCommandKind : uint8
{
    Open, SendTurn, CancelTurn, Resume
};

UENUM(BlueprintType)
enum class EGameConversationSnapshotState : uint8
{
    Idle, Active, Completed, Failed, Cancelled
};

USTRUCT(BlueprintType)
struct FGameConversationOpenResult
{
    GENERATED_BODY()

    // Authentication-session sequence for the bounded command-reply channel.
    UPROPERTY(BlueprintReadOnly) int64 ReplySequence = 0;
    // Echoes the caller's operation ID so concurrent/retried opens correlate.
    UPROPERTY(BlueprintReadOnly) FGuid ClientOperationId;
    // Valid only when PublicCode is None.
    UPROPERTY(BlueprintReadOnly) FGuid ConversationId;
    UPROPERTY(BlueprintReadOnly) EGameConversationPublicCode PublicCode{};
};

USTRUCT(BlueprintType)
struct FGameConversationCommandResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) int64 ReplySequence = 0;
    UPROPERTY(BlueprintReadOnly) EGameConversationCommandKind Command{};
    // Echoes the nonzero session-unique ID passed to the application command.
    UPROPERTY(BlueprintReadOnly) FGuid CommandId;
    UPROPERTY(BlueprintReadOnly) FGuid ConversationId;
    UPROPERTY(BlueprintReadOnly) FGuid TurnId;
    // Generic results are failures; accepted work uses the lifecycle route below.
    UPROPERTY(BlueprintReadOnly) EGameConversationPublicCode PublicCode{};
};

USTRUCT(BlueprintType)
struct FGameConversationSnapshotChunk
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FGuid ConversationId;
    UPROPERTY(BlueprintReadOnly) FGuid SnapshotId;
    UPROPERTY(BlueprintReadOnly) int64 WatermarkSequence = 0;
    UPROPERTY(BlueprintReadOnly) int32 ChunkIndex = 0;
    UPROPERTY(BlueprintReadOnly) int32 ChunkCount = 0;
    UPROPERTY(BlueprintReadOnly) int32 TotalUtf8Bytes = 0;
    UPROPERTY(BlueprintReadOnly) FGameBoundedUtf8Text ChunkBytes;
    UPROPERTY(BlueprintReadOnly) FGuid ActiveTurnId;
    UPROPERTY(BlueprintReadOnly) EGameConversationSnapshotState State{};
    UPROPERTY(BlueprintReadOnly) EGameConversationPublicCode PublicCode{};
};

USTRUCT(BlueprintType)
struct FGameConversationEvent
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FGuid ConversationId;
    UPROPERTY(BlueprintReadOnly) FGuid TurnId;
    // Monotonic across the whole conversation, not reset for each turn.
    UPROPERTY(BlueprintReadOnly) int64 Sequence = 0;
    UPROPERTY(BlueprintReadOnly) EGameConversationEventType Type{};
    UPROPERTY(BlueprintReadOnly) FGameBoundedUtf8Text Text;
    UPROPERTY(BlueprintReadOnly) EGameConversationPublicCode PublicCode{};
    UPROPERTY(BlueprintReadOnly) int32 RetryNumber = 0;
    UPROPERTY(BlueprintReadOnly) int32 MaxRetries = 0;
    UPROPERTY(BlueprintReadOnly) float RetryDelaySeconds = 0.0f;
};

UFUNCTION(Server, Reliable)
void ServerOpenConversation(FGuid CommandId);

UFUNCTION(Server, Reliable)
void ServerSendConversationTurn(
    FGuid CommandId,
    FGuid ConversationId,
    FGuid TurnId,
    const FGameBoundedUtf8Text& UserText);

UFUNCTION(Server, Reliable)
void ServerCancelConversationTurn(
    FGuid CommandId,
    FGuid ConversationId,
    FGuid TurnId);

UFUNCTION(Server, Reliable)
void ServerResumeConversation(
    FGuid CommandId,
    FGuid ConversationId,
    int64 LastAppliedSequence);

UFUNCTION(Server, Reliable)
void ServerAcknowledgeConversation(
    FGuid ConversationId,
    int64 LastAppliedSequence);

UFUNCTION(Server, Reliable)
void ServerAcknowledgeConversationSnapshot(
    FGuid ConversationId,
    FGuid SnapshotId,
    int64 WatermarkSequence);

UFUNCTION(Server, Reliable)
void ServerAcknowledgeCommandReply(int64 LastAppliedReplySequence);

UFUNCTION(Server, Reliable)
void ServerResumeCommandReplies(int64 LastAppliedReplySequence);

UFUNCTION(Client, Reliable)
void ClientReceiveConversationOpenResult(const FGameConversationOpenResult& Result);

UFUNCTION(Client, Reliable)
void ClientReceiveConversationCommandResult(const FGameConversationCommandResult& Result);

UFUNCTION(Client, Reliable)
void ClientReceiveConversationSnapshotChunk(const FGameConversationSnapshotChunk& Chunk);

// Started/retry/reset/terminal control records are reliable but still subject
// to the application acknowledgement window below.
UFUNCTION(Client, Reliable)
void ClientReceiveConversationControlEvent(const FGameConversationEvent& Event);

// Text is sequenced and recoverable by suffix replay/snapshot, so do not let
// UE's reliable actor channel become the unbounded queue.
UFUNCTION(Client, Unreliable)
void ClientReceiveConversationDelta(const FGameConversationEvent& Event);
```

Open results are command replies, not conversation events, and therefore do not consume a conversation sequence. `CommandId` is the open result's `ClientOperationId`; every accepted or rejected open echoes it, and an unsuccessful result has an invalid `ConversationId` and an allow-listed code. `SendTurn`, `CancelTurn`, and conversation `Resume` also require a nonzero client-issued `CommandId` unique within the authentication-session epoch. A syntactically valid, owned `SendTurn` that reserves its `TurnId` reports acceptance or policy failure through the sequenced conversation lifecycle. Accepted cancel/resume application commands are intentionally silent apart from the lifecycle, replay, or snapshot work they cause. Any correlatable application command rejected before it can safely enter that conversation sequence—including unknown, expired, cross-owner, bad-session, or invalid input—uses `FGameConversationCommandResult`, echoing `CommandId` plus the applicable server-issued IDs and an allow-listed failure code. Do not echo hostile numeric/payload fields and do not send both a generic reply and a conversation event for one outcome.

The three acknowledgement RPCs and `ServerResumeCommandReplies` are transport controls, not application commands. They deliberately have no `CommandId` and never generate a command reply: accepted controls only advance/replay retained state; malformed, regressing, future, wrong-owner/session, or otherwise impossible controls leave all state charged, consume a fixed protocol-violation budget, and eventually close the connection/session. This prevents acknowledgement-of-acknowledgement loops and keeps high-frequency acknowledgements out of the lifetime application-command ledger. A zero/reused-with-different-fields application `CommandId` is handled by the same silent violation path because it cannot be cached as a new correlatable result.

Every command reply carries a monotonically increasing, authentication-session-scoped `ReplySequence`. Before evaluating a syntactically valid application command, atomically reserve its `CommandId` and bounded request fingerprint in a fixed-size session command-outcome ledger. Atomically finalize its sanitized accepted/rejected outcome before or in the same transaction as any externally visible mutation; a duplicate that observes an in-progress marker attaches to that marker and never starts parallel work. Reuse the retained sequence and bytes for duplicate reply replay and never run the command mutation twice. Accepted silent commands retain a small outcome marker: a duplicate cancel replays retained active/terminal turn state, a duplicate resume reoffers the same bounded recovery state, and neither repeats application mutation. Reserve one terminal ledger slot: when normal capacity is reached, the first new command gets one retained `OperationLimit` reply and the session closes after that bounded reply attempt; no later command is evaluated or answered. Never evict within the session epoch.

Send both reply DTOs strictly in `ReplySequence` order through one per-session channel with its own fixed event/byte window, acknowledgement deadline, retained result cache, and bounded token-bucket command rate. Before invoking a reliable reply RPC, atomically charge its serialized envelope; when the send window is full, coalesce a duplicate instead of invoking the RPC again and stop sending. The client maintains one expected reply sequence per resumable authentication session, applies only a contiguous prefix, ignores duplicates, and freezes on a gap. It calls `ServerResumeCommandReplies(LastAppliedReplySequence)` to request bounded ordered suffix replay.

Authenticate `ServerAcknowledgeCommandReply` and `ServerResumeCommandReplies` against the same resumable session epoch and track the transport connection generation. On a live connection with an established delivery window, accept a gap cursor only in `LastAcknowledgedReplySequence <= LastAppliedReplySequence <= HighestContiguousReplySequenceSentToThisConnection`; replay from the cursor plus one without treating the cursor as acknowledged. On a new/replaced connection, initialize `HighestContiguousReplySequenceSentToThisConnection` to the server-retained last acknowledgement and ignore any higher client cursor for skip/release purposes: replay strictly from `ServerLastAcknowledgedReplySequence + 1`, allowing the client to discard duplicates, and advance the connection high-water mark only as that contiguous suffix is invoked in order. If the required suffix is no longer retained, invalidate the authentication session and require reauthentication rather than accepting a sequence jump; command replies are small and get no snapshot format.

For an acknowledgement, require `ServerLastAcknowledgedReplySequence <= LastAppliedReplySequence <= HighestContiguousReplySequenceSentToThisConnection`; equality is an idempotent no-op and either out-of-range direction is a silent protocol violation. Only a valid cumulative acknowledgement releases charged transport entries and advances the server-retained baseline; small logical outcome tombstones remain for the session lifetime so an acknowledged duplicate cannot become new work. An acknowledgement does not replenish the command-rate budget, so an acknowledging client still cannot produce an unbounded reliable-RPC stream. Never emit an out-of-window error in response to an out-of-window error.

Use `FGameConversationSnapshotChunk` and `ClientReceiveConversationSnapshotChunk` only for resynchronization. Give `ChunkBytes` a snapshot-specific maximum in its `NetSerialize`; validate the fixed-width `ChunkCount`, `ChunkIndex`, and `TotalUtf8Bytes` against protocol constants before allocating or indexing assembly storage. Every chunk repeats the server-issued `SnapshotId`, conversation-global `WatermarkSequence`, bounded metadata, and allow-listed active/terminal state and code. The canonical UTF-8 chunk payload encodes only the game's sanitized public transcript/partial format. Keeping snapshots separate from ordinary sequenced events makes the one legal sequence jump explicit.

`FGameBoundedUtf8Text` must implement `NetSerialize`: serialize a fixed-width byte count first, reject a count above the protocol maximum before allocation, serialize exactly that many UTF-8 bytes, reject invalid UTF-8/control policy, and set `bOutSuccess=false` on failure. Add `WithNetSerializer = true` in `TStructOpsTypeTraits`. A plain RPC `FString` followed by a `Len()` check is not a pre-allocation network bound.

Apply the same bounded serializer to event text or split text into fixed-size byte chunks. Use `FTCHARToUTF8`/`FUTF8ToTCHAR` rather than assuming `FString::Len()` is a byte count. A practical starting policy is a 4 KiB user command, 1 KiB text-delta payload, 64 KiB total response, and 20 batched delta RPCs/second, then tune from measurements.

The protocol module exposes a native bridge interface that the server module registers during startup and unregisters during shutdown. RPC implementations call that interface; they never include a server-module or UnrealAI header. Reject the RPC if the bridge is absent.

## Server conversation owner

Create one retained owner per authenticated player/conversation. It stores:

```cpp
UPROPERTY(Transient) TObjectPtr<UUnrealAIClient> Client;
TArray<FUnrealAIChatMessage> CommittedHistory;
FUnrealAIChatMessage PendingUser;
FUnrealAIRequestHandle ActiveRequest;
FGuid ConversationId;
FGuid ActiveTurnId;
int64 NextConversationSequence = 1;
bool bInFlight = false;
bool bTerminalSent = false;
FString PendingAssistantText;
FString PendingWireText;
EGameConversationPublicCode ForcedFailureCode{};
```

Keep delivery accounting per authenticated connection as well: the last validated acknowledged conversation sequence, highest conversation sequence sent, total unacknowledged conversation event/byte cost, acknowledgement deadline, and any active snapshot ID/watermark. Keep separate session-scoped last-acknowledged/highest-sent/charged/deadline accounting for command replies. These application windows are separate from Unreal's internal actor-channel state.

Only server code creates `ConversationId`. Bind it permanently to the authenticated connection/player identity and authentication-session epoch. Do not use an evicting LRU as the only proof that an operation was seen: after eviction, a replay is indistinguishable from a new ID.

Use bounded lifetime ledgers instead:

- For each authenticated session, retain every syntactically valid application `CommandId`, a bounded request fingerprint, and its sanitized accepted/rejected outcome up to the fixed command maximum described above. An exact duplicate returns/reoffers the retained outcome without repeating mutation; the same ID with different fields is a protocol violation. Keep one reserved terminal `OperationLimit` outcome slot, make that reply at most once, then close the session rather than trying to answer further new IDs. Do not evict an ID and later treat it as new. End of the authentication-session epoch invalidates its ledger and all conversation IDs.
- For each conversation, canonicalize the already bounded valid user payload, then atomically reserve every syntactically valid `TurnId` before mutable policy checks. Bind the reservation to the exact bounded canonical UTF-8 bytes plus immutable request-version/profile fields (or retain those bytes with a keyed digest) and keep that fingerprint with its sanitized active/terminal record up to the fixed turn maximum. A new `CommandId` that reuses a `TurnId` must compare this fingerprint before any mutation: an exact active duplicate reattaches, an exact terminal duplicate replays, and a mismatch gets a cached generic `InvalidRequest` outcome for the new command. Busy, rate, moderation, circuit, budget, and other preflight rejections become terminal turn records too. When the ledger is full, reject new IDs with `OperationLimit` or expire the whole conversation before removing the ledger.
- Large terminal payloads/event suffixes may expire earlier, but their small ID tombstones and keyed canonical turn fingerprints remain for the ledger lifetime. An exact duplicate whose payload expired returns `TurnExpired`; a changed-field reuse returns `InvalidRequest`; neither starts a new provider request. An RPC for a server-issued conversation that is no longer retained returns `ConversationExpired` and cannot create a conversation implicitly.

Suggested starting caps are 8 conversations, 1,024 application commands (plus the one terminal slot) per authentication session, and 256 turns per conversation. Tune them from product requirements while preserving the no-eviction-within-lifetime invariant.

Before start, authenticate and check conversation ownership, validate nonzero IDs and the bounded wire representation, canonicalize the UTF-8 input/control policy, compute the bounded turn fingerprint, then atomically reserve or compare the `TurnId`. After reservation, check history/token/output bounds, per-player/fleet concurrency, rate and cost budgets, moderation policy, circuit state, and an absolute deadline. Record every outcome against both the command and turn reservations. Select provider/model/system prompt only from trusted server configuration.

Build the request from `CommittedHistory` plus a separate `PendingUser`. Set all active state before calling the SDK because preflight failure can invoke the terminal synchronously:

```cpp
FUnrealAIChatRequest Request;
Request.Messages = CommittedHistory;
Request.Messages.Add(PendingUser);
Request.bUseMaxCompletionTokens = true;
Request.MaxCompletionTokens = Policy.MaxOutputTokens;
Request.RetryOptions.Mode = EUnrealAIRetryMode::OverrideMaxRetries;
Request.RetryOptions.MaxRetries = Policy.MaxRetries;

BeginActiveTurn(TurnId);       // sets bInFlight and bTerminalSent=false
EmitStarted();
StartAbsoluteDeadline();       // arm before submission; terminal may be synchronous
const FUnrealAIRequestHandle Started = Client->StreamChatCompletion(
    Request,
    FUnrealAIChatStreamEventNativeDelegate::CreateUObject(
        this, &ThisClass::HandleStreamEvent),
    FUnrealAIChatStreamTerminalNativeDelegate::CreateUObject(
        this, &ThisClass::HandleTerminal),
    FUnrealAIRetryNativeDelegate::CreateUObject(
        this, &ThisClass::HandleRetry));

if (bInFlight)
{
    ActiveRequest = Started;
    if (!ActiveRequest.IsValid())
    {
        FinishOnceAsFailure(EGameConversationPublicCode::RequestNotStarted);
    }
}
```

Call the SDK only on the game thread. UnrealAI delivers event, retry, and terminal delegates on the game thread; preflight and cancellation terminal callbacks may be synchronous.

## Stream, retry, and commit rules

- Accept only `TextDelta`. Never forward `ProviderEvent` or `RawJson`.
- Normalize transport text, then apply a separately documented display policy. “Sanitized” must mean: valid UTF-8; bounded bytes; disallowed C0 controls removed except the game's permitted whitespace; bidi/control policy applied; and safe for the selected renderer. It does not mean moderated or trustworthy.
- If moderation must occur before display, buffer until the moderation decision; token streaming and pre-display moderation are otherwise incompatible.
- Append within the total output cap and batch deltas by byte count/cadence. Set `ForcedFailureCode` before `CancelRequest`, because cancellation may invoke terminal synchronously.
- Retry is progress on the same logical turn. Emit only bounded ordinal/count/delay. Do not change history or create a new turn ID.
- On successful terminal, require a nonempty bounded aggregate. If it differs from streamed UI text, emit `TextReset` and a bounded snapshot before `Completed`.
- Commit `PendingUser` plus exactly one assistant message only after success. Failure, cancellation, timeout, output breach, or empty completion discards `PendingUser`; partial text remains display-only.
- Gate all terminal paths with `bTerminalSent`, clear the absolute deadline, flush or discard the delta queue according to policy, and ignore late callbacks.
- Explicit user retry after terminal creates a new `TurnId`. Never automatically replay a partially delivered stream.

UnrealAI retries a stream only before the first complete SSE event, not merely before visible text. Cancellation also works during retry backoff. These SDK guarantees do not replace the application turn-ID cache.

Map application-forced failures before invoking cancellation so a synchronous SDK cancellation terminal cannot replace the intended public result:

```cpp
void AbortActiveTurn(EGameConversationPublicCode PublicCode)
{
    if (!bInFlight || bTerminalSent)
    {
        return;
    }

    ForcedFailureCode = PublicCode;
    const bool bAccepted = Client->CancelRequest(ActiveRequest);
    if (bInFlight && !bAccepted)
    {
        FinishOnceAsFailure(ForcedFailureCode);
    }
}

void HandleTerminal(const FUnrealAIChatStreamResult& Result)
{
    if (!bInFlight || bTerminalSent)
    {
        return;
    }

    if (ForcedFailureCode != EGameConversationPublicCode::None)
    {
        FinishOnceAsFailure(ForcedFailureCode);
        return;
    }

    // Handle provider success, failure, and user cancellation here.
}
```

Use `AbortActiveTurn(EGameConversationPublicCode::Timeout)` from the absolute-deadline path and `AbortActiveTurn(EGameConversationPublicCode::OutputLimit)` from the aggregate-bound path. `FinishOnceAsFailure` owns clearing `ForcedFailureCode`, the timer, the request handle, pending input, and queues.

Use this deterministic public mapping:

- valid nonempty bounded aggregate after SDK `Completed` -> `Completed/None`;
- empty or invalid aggregate -> `Failed/EmptyResponse`;
- explicit authenticated player cancel -> `Cancelled/None`;
- SDK `Cancelled` while `ForcedFailureCode` is set -> `Failed/<forced code>`;
- absolute application deadline -> `Failed/Timeout`;
- aggregate/output queue breach -> `Failed/OutputLimit`;
- outbound client queue breach -> `Failed/ClientBackpressure`;
- SDK `Failed`, provider authentication/configuration failure, exhausted retry, or unknown transport failure -> `Failed/ServiceUnavailable`;
- application preflight -> `Failed` with its exact allow-listed authorization, busy, rate, moderation, or limit code.

Provider timeouts remain `ServiceUnavailable` unless the consuming game deliberately exposes a separate provider-timeout code. Lifecycle cancellation after disconnect/travel is retained internally for resume/audit but is emitted only if a valid client channel still exists.

## Ordering, reconnect, and teardown

Increment the conversation-global sequence for every accepted event. On the client, track one expected sequence per conversation, ignore duplicates, and freeze event application on a gap. Request `ServerResumeConversation(CommandId, ConversationId, LastAppliedSequence)`. Track the delivery connection generation and a server-retained last acknowledged sequence for that identity/conversation. On the same live connection with an established window, accept the client cursor only between that trusted acknowledgement and the highest contiguous sequence sent on that connection, then replay from `LastAppliedSequence + 1` without advancing acknowledgement. On a new/replaced connection, treat `LastAppliedSequence` only as an untrusted diagnostic hint: initialize the new window's contiguous-sent watermark to `ServerLastAcknowledgedSequence`, replay from `ServerLastAcknowledgedSequence + 1` regardless of the higher client cursor, and let the client discard duplicates. Advance the new connection watermark only while invoking that replay contiguously. If the required trusted-baseline suffix is unavailable, use the snapshot path below; never skip records merely because a reconnecting client claims to have applied them.

After applying ordinary events contiguously, the client periodically sends `ServerAcknowledgeConversation(ConversationId, LastAppliedSequence)` and sends it immediately after a terminal/control record. Authenticate the caller and conversation/session epoch and require `ServerLastAcknowledgedSequence <= LastAppliedSequence`, `LastAppliedSequence <= CurrentConversationWatermark`, **and** `LastAppliedSequence <= HighestContiguousSequenceSentToThisConnection`. Reject the acknowledgement when either upper bound is exceeded; an equal acknowledgement is an idempotent no-op. An acknowledgement is cumulative: only after it validates may the server advance its retained trusted baseline, release acknowledged replay records, and decrement their charged event/byte cost. A resume request is not an implicit acknowledgement, and acknowledgements never create or retarget conversations. Replaying the same retained logical event does not charge its payload twice, but every resend has a small fixed attempt budget and shares the acknowledgement deadline; never loop unreliable or reliable RPC invocations while an acknowledgement is stalled.

If the suffix is unavailable but the conversation remains resumable, capture one immutable public snapshot. It contains the bounded sanitized display transcript selected by the game (never provider history), the active turn ID and bounded partial text if any, the latest terminal state/code, and a conversation-global watermark. Encode it as canonical UTF-8 and split it into bounded chunks under one snapshot ID. The client enters resync mode and accepts only chunks with that ID/watermark, consecutive bounded indices, and a bounded total byte count. After every chunk arrives, atomically replace display/terminal state, set `LastAppliedSequence = WatermarkSequence`, discard queued ordinary events at or below that watermark, and resume ordinary application at `WatermarkSequence + 1`. Only this validated snapshot may cross a gap.

Use an assembly timeout and cap both snapshot bytes and post-watermark buffered events (for example, 64 KiB, 10 seconds, and 64 events/64 KiB). Mixed, duplicate, oversized, incomplete, or timed-out snapshots are discarded and the same immutable snapshot chunks may be retried once within their existing charge; repeated failure expires the local view and requires reconnect/restart. After atomically applying a snapshot, the client sends `ServerAcknowledgeConversationSnapshot(ConversationId, SnapshotId, WatermarkSequence)`. Accept it only from the bound identity/session epoch when both values exactly match the server's active immutable snapshot and its watermark is not ahead of the conversation. Only that acknowledgement completes resync, atomically advances the server-retained trusted acknowledgement and current connection's contiguous watermark to the snapshot watermark, releases replay/snapshot state at or below it, and opens the post-watermark send window. A wrong, stale, repeated, or future snapshot acknowledgement cannot release state. If the server's post-watermark buffer overflows during transfer, set `ClientBackpressure`, cancel the active provider turn, expire the conversation, and tear down its delivery window; do **not** supersede the unacknowledged snapshot in place. Teardown discards its charges as undelivered transport state, not as acknowledged application state. If neither replay nor snapshot state is retained, return `ConversationExpired` through the bounded command-reply path and require an explicit restart.

When suffix replay is impossible, first serialize the complete immutable snapshot and compute the charge for **all** chunks and RPC envelopes. Atomically reserve that entire charge in a dedicated recovery quota whose byte/event caps are no larger than the connection window. Entering snapshot mode then replaces—not releases as delivered—the old ordinary-event charges at or below the snapshot watermark with this fully reserved snapshot charge. If the complete snapshot cannot fit, shrink only at documented whole transcript-unit boundaries and recompute it; if the minimum valid snapshot still cannot fit, expire the conversation without invoking any snapshot RPC. Retain the immutable source/snapshot record and its full charge until its exact acknowledgement or terminal delivery-window teardown; teardown never marks it delivered. Because every chunk is reserved before the first reliable invocation, the server can send the whole bounded snapshot without waiting for an acknowledgement that the client cannot produce until assembly completes. Post-watermark events remain separately capped and cannot send until snapshot acknowledgement.

Treat resume/cancel fields as hostile. Reject invalid conversation/turn IDs and cross-owner requests. `LastAppliedSequence` may be zero or a retained past sequence; reject negative values and values greater than the server's current watermark. A stale cancel for a terminal retained turn replays its terminal state; an unknown/expired turn returns `TurnExpired`; it never targets the currently active turn by implication.

Reliable RPC order applies only to the same actor channel and live connection. Sequence IDs, explicit acknowledgements, trusted-baseline replay, and resume snapshots cover packet loss, reconnect, actor replacement, travel, and duplicate application dispatch. A reconnect creates a new connection delivery window beginning at the server-retained acknowledgement; it does not inherit unvalidated acknowledgement claims from the client.

Do not use reliable RPCs for streaming text deltas. Retain each sequenced delta in the bounded replay cache, send it through `ClientReceiveConversationDelta`, and charge it to a per-connection sliding window before invoking the RPC. Reliable control/terminal records are charged to that window before invocation. Snapshot chunks use the atomically reserved recovery quota described above, and command replies use their separate bounded session window, so no reliable path can feed Unreal's actor channel without an application-level bound. Stop invoking ordinary outbound RPCs when their window is full; buffer only the already bounded aggregate/post-watermark state needed for recovery. Resume sends only after a validated cumulative acknowledgement frees capacity.

Start a short acknowledgement grace deadline when the window first stalls. If it expires, or bounded pending/recovery state fills first, set `ForcedFailureCode = ClientBackpressure` and cancel the provider request through the forced-failure path. Retain only the bounded terminal tombstone/snapshot permitted by policy. If snapshot application is awaiting acknowledgement, retry its immutable chunks only within a fixed retry/byte budget; a stalled snapshot acknowledgement also reaches `ClientBackpressure`/conversation expiry. If even the terminal/snapshot channel cannot drain, close or expire the conversation; never continue generating provider output for an unreachable slow client. Do not infer delivery from successful RPC invocation—only a validated acknowledgement advances the window.

Use a fixed teardown order: stop accepting new RPCs; make the bridge reject calls and unregister it; mark owners as closing; cancel every active request while owners/delegates are still retained; let synchronous terminals pass through the terminal gate; clear timers and outbound queues; persist or discard bounded terminal ledgers according to policy; then release owners, clients, and module state. Apply the same owner-level subset on logout, disconnect, seamless/non-seamless travel, deadline, or owner destruction. Emit a client-visible cancellation only when its connection/lifecycle still permits it. `BeginDestroy` is fallback cleanup, not a reliable product notification point.

## Credential and log boundary

Inject provider credentials into the dedicated-server process from the deployment secret manager. Use `ConfigureFromSettings` or a trusted native config; never accept provider/model/base URL/header/native JSON choices from an RPC. Never copy `.env` into a container layer, cooked config, crash bundle, or artifact. Do not log provider configuration, prompts, responses, raw errors, headers, environment values, or session tokens.

## Required verification

Before calling the integration production-ready:

1. Build Shipping Game and Shipping Server targets on every supported native host.
2. Inspect Game receipts/imports and run a Shipping client without UnrealAI present. Assert it does not link/load `GameConversationServer` or UnrealAI and contains no provider endpoint/model/key-variable strings introduced by server code.
3. Inject a synthetic secret only into the Server process and scan source, cooked config, client/server artifacts, images, logs, and crash output; it may appear nowhere persistent.
4. Test correlatable duplicate/rejected conversation opens, authentication-session and operation limits, unauthorized ownership, invalid/oversize serialized input, atomically reserved preflight failures, replayed active/terminal/payload-expired turn IDs, and new-command reuse of each such turn ID with changed user text or immutable fields; mismatches must cache `InvalidRequest` and never mutate. Also cover per-player/fleet concurrency, rate/cost rejection, circuit-open behavior, and moderation rejection.
5. Test invalid/future/negative resume watermarks; spoofed, regressing, repeated, and future ordinary acknowledgements; the case where the global watermark exceeds this connection's highest-sent sequence; wrong-owner/session acknowledgements; non-acking clients; bounded send-window pause/resume; receipt and acknowledgement of accepted/rejected opens plus every generic pre-sequence application-command rejection kind; stable reply sequences on duplicate replay; reused command IDs with changed fields; command-reply flooding with duplicate/new/expired IDs (with and without acknowledgements), strict ordered application, live-gap replay, reconnect replay from the server acknowledgement despite a higher client cursor, suffix-unavailable reauthentication, coalescing, terminal-slot closure, rate/violation closure, and silent invalid transport controls; sequence duplicates/gaps; conversation live-gap replay; reconnect replay from the trusted server baseline; snapshot fallback when that baseline suffix is unavailable; bounded snapshot-chunk `NetSerialize`; immutable chunked snapshots; atomic whole-snapshot reservation at and above the recovery quota; wrong/stale/future snapshot IDs and watermarks; stalled snapshot acknowledgement; release only after validated acknowledgement; assembly timeout; post-watermark overflow causing expiry without supersession; delta batching/queue caps; slow-client `ClientBackpressure` cancellation; final-text reconciliation; and no commit of partial text.
6. Test stale/unknown cancels, disconnect, travel, deadline, retry exhaustion, cancellation during backoff, cancellation after partial SSE, provider outage, every public status mapping, late/duplicate terminal callbacks, and the specified shutdown order.

The repository's sample contracts prove UnrealAI request/lifetime semantics and a narrow credential-free packaged-client facade. They cannot prove another game's target layout, RPC ownership, deployment secret manager, or abuse policy. Add the protocol/server modules and the tests above to the consuming game; do not cite `Deployment.Configuration` alone as production evidence.
