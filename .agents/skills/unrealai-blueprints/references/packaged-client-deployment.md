# Packaged-Client Blueprint Deployment

Use this reference for a Blueprint-facing feature shipped in a player-controlled game process. A packaged client or listen server calls a native game-backend facade or validated Server RPC; it must not call hosted providers with a long-lived key. Use `dedicated-server-deployment.md` for controlled servers and `backend-deployment.md` for external services.

For a packaged Blueprint UI with bounded history and backend streaming, load `backend-streaming-conversation.md` instead; it is the canonical combined implementation recipe.

UnrealAI does not enforce the credential boundary or provide player authentication, authorization, moderation, quotas, durable history, or circuit breaking. `Switch Has Authority` controls gameplay authority; it does not turn a listen server or cooked asset into a trusted credential store.

## Recipe: packaged Blueprint client

Use this graph shape:

```text
Widget/Actor Blueprint
    -> native Game AI facade or validated Server RPC
    -> trusted backend/dedicated server
    <- sanitized Completed / Failed / Cancelled / Retrying or stream events
```

1. Expose a narrow native `BlueprintCallable` operation such as `Send Dialogue Turn`, not `Configure Provider` or unrestricted raw request fields.
2. Authenticate through the game's existing session flow. The native facade may hold a short-lived, revocable backend token in memory, but it must not expose that token or its `FUnrealAIProviderConfig` to Blueprint.
3. Let the trusted service choose the real provider, model, system prompt, limits, and allowed parameters. Treat Blueprint input as untrusted even when the graph is part of the shipped game.
4. Return sanitized domain values or normalized text and lifecycle state. Do not return authorization headers, upstream request IDs, prompts from other users, `Provider Event`, or raw JSON by default.
5. Disable submission while a serialized conversation request is active and cancel it when the Widget closes or the owning gameplay object is destroyed.

Do not place `OPENAI_API_KEY`, `XAI_API_KEY`, `ANTHROPIC_API_KEY`, `GEMINI_API_KEY`, a long-lived bearer token, or sensitive `Additional Headers` in Class Defaults, data assets, project settings, config variables, save games, screenshots, or node defaults. Obfuscation and an authority branch do not make a client-owned secret safe.

The current Blueprint surface cannot securely acquire and rotate a short-lived backend token by itself. Use a native facade for a packaged-client proxy until the SDK has a non-reflected credential-provider abstraction.

## Treat model output as untrusted

Never connect model text or tool-like JSON directly to console commands, asset/file paths, class loading, purchases, inventory grants, moderation actions, matchmaking, server administration, or arbitrary replicated state. Convert output into a narrow schema, validate every field, re-check player authorization, clamp values, and let trusted gameplay code decide whether to apply the action.

## Verification

- Package Shipping without provider environment variables and scan staged assets/config for keys, authorization headers, and provider overrides.
- Confirm the Blueprint graph has no direct hosted-provider node path and receives only allow-listed facade/controller events. Define "sanitized" per field: bounded/canonical text, public error-code allow-list, and no raw provider metadata.
- Exercise invalid, expired, revoked, replayed, and over-budget game sessions.
- Confirm prompts, responses, raw JSON, and credentials stay out of logs and crash reports.

The repository's production-readiness document lists unresolved SDK-level gates. Do not claim that UnrealAI itself prevents insecure Shipping-client configuration until those gaps are closed and tested.

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` after changing this boundary. The native suite regenerates and recompiles the real packaged-chat Widget Blueprint in an isolated copy, checks all six reflected presentation/readiness event routes and its empty layout tree, and exercises `Blueprint.BackendFacade` plus `Blueprint.PackagedWidgetBehavior` without real credentials. The same runner also requires the core plugin wire/retry contracts declared in `skill_contracts.json`.
