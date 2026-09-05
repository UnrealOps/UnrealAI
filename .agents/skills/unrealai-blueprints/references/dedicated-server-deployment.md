# Dedicated-Server Blueprint Deployment

Use this reference only when a Blueprint runs on infrastructure controlled by the game operator. A listen server remains player-controlled and must follow `packaged-client-deployment.md`.

## Server-owned graph

1. The client Widget sends a bounded gameplay request to an authoritative server object through the game's validated RPC layer.
2. Server code authenticates the player, authorizes the feature, validates prompt/history, and applies per-player and fleet admission limits.
3. A server-owned Blueprint service calls `Create Chat Completion (UnrealAI)`, `Stream Chat Completion (UnrealAI)`, or `UnrealAIChatComponent`. The provider key comes from the dedicated-server process environment and never appears in the graph.
4. Handle all terminal paths. Cancel when the player disconnects, the world travels, the actor is destroyed, or the response is no longer needed.
5. Convert output into an allow-listed domain result in trusted server code and return only authorized, sanitized data to the owning client.

Keep provider-calling assets out of client cooks when possible, using server-only modules or content boundaries. Even if a graph is present in a client package, no provider credential or sensitive header may be stored in it.

Retries, stream queue limits, and Unreal authority are not rate limiting. Enforce request frequency, concurrency, prompt and response size, total deadlines, token/cost budgets, and outage behavior before invoking UnrealAI.

## Verification

- Build a Shipping Server target and inject a synthetic key only through the process environment.
- Verify the key is absent from source, assets, cooked config, containers, and artifacts.
- Test disconnect, map travel, cancellation, timeout, retry exhaustion, admission rejection, and provider outage behavior.
- Confirm listen servers and packaged clients cannot execute the credentialed path.

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` for the credential-free provider configuration contract. A real server qualification still requires the consuming game's Server target and RPC/security tests.
