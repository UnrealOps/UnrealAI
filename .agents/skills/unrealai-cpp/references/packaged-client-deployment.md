# Packaged-Client Deployment

Use this reference when UnrealAI code or UI ships in a player-controlled game process. A packaged client and listen server must call a trusted game backend or controlled dedicated server; neither may receive a long-lived hosted-provider key. Use `dedicated-server-deployment.md` or `backend-deployment.md` for those trusted runtimes.

UnrealAI supplies provider request construction, HTTP transport, streaming normalization, retries, and cancellation. It bounds request admission and exposes native circuit-breaker mechanisms. Player authentication, authorization, moderation, quotas, and durable history remain application responsibilities.

## Shipping migration

The convenience `ApiKeyOverride` path below is now a development configuration example: Shipping-client admission rejects it even when the value is a game-session token. Production clients must use the game's authenticated network service or the native provider SPI with explicit `GatewayBearer` / `GatewayAccounted` connection policy. See [native SDK integration](native-sdk.md). The compile-tested sample validates configuration construction; it does not establish Shipping dispatch support for this legacy path.

## Development configuration for an OpenAI-compatible proxy

Use a native game service or subsystem as the only UnrealAI owner. UI and gameplay code call its sanitized methods; they do not receive `FUnrealAIProviderConfig`, credentials, authorization headers, or raw provider JSON.

The repository's compile-tested boundary is `FUnrealAIProductionDeploymentExample` under `Samples/UnrealAISample/Source/UnrealAISample`. Adapt it into the game's private service rather than depending on the sample module.

Obtain a short-lived, audience-scoped token from the game's existing authentication flow, then construct the proxy profile in memory:

```cpp
FUnrealAIProviderConfig BackendConfig;
BackendConfig.Name = TEXT("GameAIBackend");
BackendConfig.Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
BackendConfig.BaseUrl = TEXT("https://ai.example.invalid/v1");
BackendConfig.DefaultModel = TEXT("game-chat");
BackendConfig.bRequiresApiKey = true;
BackendConfig.ApiKeyEnvironmentVariable.Reset();
BackendConfig.ApiKeyOverride = ShortLivedGameSessionToken;
BackendConfig.TimeoutSeconds = 30.0f;

FUnrealAIError ConfigurationError;
UnrealAIClient = UUnrealAIProviders::OpenAICompatible(
	this,
	BackendConfig,
	ConfigurationError);
```

`ApiKeyOverride` becomes a bearer `Authorization` value for the OpenAI-compatible adapter. In a packaged client it may contain only a revocable game-backend session token, never an OpenAI, xAI, Anthropic, Gemini, or other upstream provider secret. The current field is reflected and the SDK does not provide secure token storage, rotation, certificate pinning, or host allow-list enforcement; keep the config private to native code, avoid crash/log capture, and recreate or reconfigure the retained client when the session token changes and no request is active.

The proxy must expose `<BaseUrl>/chat/completions`. One-shot responses must use the OpenAI-compatible Chat Completions JSON shape; streams must use compatible SSE frames and terminate cleanly. If the backend uses a game-specific contract instead, call it through the game's HTTP/network layer rather than pretending it is an UnrealAI provider.

The backend, not the client, validates the session, selects the real provider/model and prompt version, enforces budgets, and strips sensitive metadata. Do not let client-provided `Model`, `AdditionalHeaders`, `AdditionalParametersJson`, tools, or system instructions bypass backend policy.

## Verification

- Build Shipping without provider environment variables and inspect the staged package, cooked config, logs, symbols, and assets for keys or authorization values.
- Verify the client can reach only the approved HTTPS backend in normal configuration and that upstream provider domains are not required.
- Exercise expired, revoked, replayed, wrong-audience, and over-budget game tokens.
- Confirm prompts, raw responses, credentials, and provider metadata do not enter logs or crash reports.

The repository's production-readiness document identifies unresolved SDK-level gates. Admission rejection does not remove secrets already serialized in legacy assets; inspect and migrate those assets before distributing a client.

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` to compile the packaged-client helper and execute `UnrealAISample.SkillContracts.Deployment.Configuration`. Full plugin CI also requires `UnrealAI.Providers.ProtocolAdapters`, which constructs the proxy URL, bearer header, JSON body, and SSE mode in memory without a network request.
