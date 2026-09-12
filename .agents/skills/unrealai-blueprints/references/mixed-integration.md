# Mixed Blueprint and C++ Integration

Use this reference when Blueprint and native code share an UnrealAI feature. Choose exactly one owner for conversation history and request state.

## C++ owns state; Blueprint owns UI

Use the compile-tested `AUnrealAIMultiTurnExample` from `Samples/UnrealAISample` when native code should own provider access, request handles, history bounds, and rollback.

### Actor Blueprint

1. Create a Blueprint subclass of `AUnrealAIMultiTurnExample`.
2. Set `Provider Name`, `System Prompt`, and `Max Retained Turns` in Class Defaults. Do not add an `UnrealAIChatComponent` to this actor.
3. Bind `On Turn Completed`, `On Turn Failed`, `On Turn Cancelled`, and `On Turn Retrying` once.
4. Call `Reset Conversation` at the session boundary.
5. Send input through `Send Turn`. If it returns false, handle `Out Error` as a local rejection. Disable submission while `Request In Flight` is true.
6. Render `Assistant Text` on completion, `Error.Message` on failure, cancellation separately, and retry data only as progress.
7. Call `Cancel Turn` for user cancellation.

`Conversation History` is read-only to Blueprint. Do not maintain a shadow array or append messages in event handlers.

### Widget Blueprint

Store a reference to the native owner and bind custom events with matching signatures once during initialization. Release or unbind it through the Widget's normal lifecycle. Submit through `Send Turn`; do not create another UnrealAI client or async action.

For a dedicated server, keep the native owner in authoritative server code. Client UI calls a validated game RPC; the server returns only authorized, sanitized response state. Do not replicate full conversation history unless every message is safe for that client.

When the native owner uses the model-provider SPI, display its sanitized `FUnrealAIModelError::UserMessage` on failure. Native HTTP failures include a numeric status and may include a recognized SDK-authored explanation. Keep raw response bodies, private diagnostic metadata, and credentials out of Blueprint UI. Endpoint-specific wire options belong to the native provider configuration, not the widget graph.

## Blueprint owns state; C++ supplies helpers

Choose this boundary when designers must own the request graph:

1. Keep history, pending-turn state, and terminal handling in Blueprint.
2. Expose narrow native `BlueprintCallable` functions for domain actions.
3. Pass parsed or validated values rather than `Provider Event` or raw JSON.
4. Do not let the helper maintain a second conversation history.

The generated `BP_UnrealAIGettingStarted` demonstrates an async node calling C++ terminal sinks, although it is intentionally single-turn.

## Verification

Compile the Blueprint and verify the routes used by its request mode: four one-shot bindings (`Completed`, `Retrying`, `Failed`, and `Cancelled`) or five streaming bindings (`Text Delta`, `Retrying`, `Completed`, `Failed`, and `Cancelled`). Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` on a matching host: `Mixed.BlueprintSurface` validates reflected commands, state, and delegates, while `Cpp.StreamingConversationBehavior` protects combined streaming and native history ownership.
