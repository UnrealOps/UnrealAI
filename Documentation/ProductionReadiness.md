# Production readiness

This document describes the SDK consolidation working tree based on version 0.6.1. It does not change release versions or claim a production release.

| Area | Implemented | Remaining qualification or application responsibility |
| --- | --- | --- |
| Reusable model access | Native convenience service, provider SPI, protocols, typed tools/results, usage, continuations | The portable convenience and strict native paths have different guarantees; no automatic tool executor |
| Credentials | Move-only leases, exact destinations, redacted inspection, optional stores/OAuth, Shipping-client provider-secret rejection | Migrate secrets already serialized in legacy assets; game authentication and backend remain application-owned |
| Bounded execution | Admission/body/event/queue limits, monotonic deadlines, cancellation, exactly-once logical terminals, physical-settlement accounting | Game-specific load/soak and per-frame delivery budgets |
| Provider protocols | Responses, compatible Chat, Anthropic, generateContent convenience, explicit native Interactions, inline images | Audio, embeddings, new hosted tools, background work, live API/model qualification |
| Extension and deployment | Independent base SDK, optional addons, injected provider/broker/transport interfaces, client/server contracts | Native strict HTTPS transport currently implemented on Mac; Win64/Linux implementations and qualification remain |
| Reliability | Shared retry/quota classification and circuit mechanisms, convenience retries, native single-attempt contract | Agent budget charging, routing/fallback, and fleet policy belong to the caller |
| Tests and distribution | Offline regression fixtures, strict base packaging, standalone consumer preparation, source addon packaging | Exact release-commit qualification on every supported host; no release/tag created by this migration |
| Observability | Typed usage, IDs, classified errors, retry and terminal events | Application telemetry export, cost accounting, alerting, and dashboards |

The existing C++ factories and reflected Blueprint node names are preserved. The native SDK model SPI is additive; the extraction adds an UnrealAI dependency to AutonomousAgents. See [Native SDK](NativeSDK.md) for the ownership boundary and [CI](ContinuousIntegration.md) for repeatable validation.

The convenience HTTP service and strict native provider SPI are separate execution surfaces. The native SPI provides destination-bound, one-shot credential application and physical callback settlement. Portable convenience clients retain the existing FHttpModule path and game-thread delegate contract. Do not infer strict transport guarantees from convenience API tests.

Shipping client admission rejects direct provider keys or OAuth bearer credentials. For authenticated game clients, use a game network service or native `GatewayBearer` / `GatewayAccounted` connection. The legacy convenience `ApiKeyOverride` proxy recipe only constructs development configuration; it is rejected by Shipping-client admission. A trusted dedicated server can own hosted-provider credentials. No SDK can remove secrets already placed in a cooked asset or establish multiplayer authority by itself.

Release qualification must include matching native host builds, client/server target exclusions, copied-plugin consumers, source and package credential scans, and offline regression reports. Mac results do not prove Windows/Linux support. The game still owns authorized tools, memory retention, player/fleet budgets, privacy, content policy, and graceful behavior during provider outages.
