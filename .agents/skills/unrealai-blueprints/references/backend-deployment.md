# Blueprint UI with an External Backend

Use this reference when Blueprint talks to a game service and the backend owns hosted providers, credentials, model selection, prompts, moderation, and budgets.

For the complete packaged Blueprint multi-turn streaming graph and compile-tested facade, load `backend-streaming-conversation.md` instead of combining this reference manually with the async-action and history references.

## UI and facade flow

```text
Widget or Actor Blueprint
    -> native game AI facade
    -> authenticated backend
    <- sanitized lifecycle events or normalized text
```

1. Obtain the game AI service during initialization and bind its sanitized lifecycle delegates once.
2. Submit a feature identifier and bounded user/domain input. Do not send a provider key, arbitrary provider URL, unrestricted model name, or unvalidated tool definition.
3. Let the backend validate session and entitlement, choose a server-owned model alias and versioned system prompt, and apply safety and budget policy.
4. For streaming, expose normalized text deltas and exactly one terminal state. Keep partial text display-only until the selected commit policy accepts the response.
5. On failure, cancellation, timeout, or disconnect, clear pending UI state exactly once. Do not add a Blueprint replay loop after partial output.

If the backend implements OpenAI-compatible Chat Completions, a native facade may configure UnrealAI with its HTTPS base URL. The service must expose `/chat/completions` and compatible JSON/SSE. For a custom game protocol, use the game's networking layer and keep UnrealAI behind the trusted service boundary.

Treat model output as untrusted. Parse it into a narrow schema, validate fields, re-check authorization, clamp values, and let trusted gameplay code decide whether to apply an action.

## Verification

- Contract-test one-shot JSON and SSE event/terminal behavior through the deployed proxy.
- Exercise invalid, expired, revoked, replayed, and over-budget sessions.
- Load-test quotas, bounded relay buffers, cancellation propagation, and circuit-breaker recovery.
- Verify redaction with synthetic secrets and private prompts.
- Version, evaluate, stage, and roll back model and prompt changes independently of the game binary.

The repository's `UnrealAI.Providers.ProtocolAdapters`, `UnrealAISample.SkillContracts.Deployment.Configuration`, and `UnrealAISample.SkillContracts.Blueprint.BackendFacade` tests validate the SDK-facing compatible protocol, proxy configuration, narrow Blueprint surface, message bounds, text-only events, and terminal cleanup offline. They do not replace deployed backend security or load tests.
