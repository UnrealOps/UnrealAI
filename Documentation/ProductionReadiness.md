# Production readiness

This document tracks the work required to make UnrealAI suitable for broad adoption in production games. It describes the state of version `0.4.0`; update the version, status, and evidence as the plugin evolves.

UnrealAI is currently appropriate for prototypes, internal tools, and bounded text-generation features that run through a trusted backend or dedicated server. It should not yet be presented as a turnkey production SDK for an untrusted packaged client.

## Priority and status

Priorities describe adoption impact:

- **P0**: an active security, data-loss, or fundamental correctness issue that prevents use until resolved;
- **P1**: a production launch gate for broadly distributed games;
- **P2**: a material reliability, compatibility, or adoption gap that should be resolved before a stable `1.0.0` release.

Statuses used below are `Open`, `Partial`, and `Complete`. A gap is complete only when its acceptance criteria are enforced by automated validation or supported by documented platform evidence.

No P0 issue is known at the time of this review. The absence of a known P0 is not a security guarantee; repeat this review whenever authentication, transport, serialization, or Blueprint-facing contracts change.

## Summary

| Priority | Gap | Status | Target outcome |
| --- | --- | --- | --- |
| P1 | Enforced credential boundary | Open | A packaged game client cannot accidentally contain or expose a hosted-provider secret. |
| P1 | Release-gated native validation | Open | Every release is qualified by required Unreal compilation and tests on each supported host. |
| P1 | Runtime resource and latency budgets | Partial | Requests have explicit memory, concurrency, per-frame work, and absolute-time limits. |
| P1 | Current provider API abstraction | Partial | Responses text/tools/continuation implemented; media, hosted tools, Interactions, and background work remain. |
| P2 | End-to-end HTTP lifecycle tests | Partial | Responses loopback contracts exercise native/Blueprint HTTP; exhaustive timeout, destruction, and race coverage remains. |
| P2 | Normalized safety and refusal outcomes | Partial | Responses exposes explicit refusal parts and incomplete status; broader provider safety normalization remains. |
| P2 | Provider extension interface | Open | A new native wire protocol can be registered without editing or forking the UnrealAI module. |
| P2 | Platform and engine compatibility evidence | Partial | Support claims are backed by repeatable client, editor, server, and host-platform results. |
| P2 | Production observability and fleet controls | Open | Integrators can monitor latency, retries, limits, usage, and provider request IDs without logging content. |
| P2 | Versioned distribution and support policy | Open | Releases include consumable artifacts, compatibility metadata, migration guidance, and a support path. |

## P1: Enforce the credential boundary

### Current state

`FUnrealAIProviderConfig` exposes `ApiKeyOverride` and `AdditionalHeaders` as editable and Blueprint-visible properties. Provider profiles are stored through `UDeveloperSettings` with `Config = Game`, so values entered in project settings can be serialized into project configuration. Blueprint code can also retrieve the resolved provider configuration. The project-root `.env` loader is available in runtime builds.

The README correctly warns users not to ship provider secrets, and a dedicated server or trusted backend can own the credentials. The SDK does not enforce that deployment boundary.

### Production risk

A developer can unintentionally place an API key or authorization header in source control, a cooked configuration file, a Blueprint-visible value, crash diagnostics, or a distributed client. A client-owned key can be extracted and used outside the game, creating unauthorized spend, quota exhaustion, data exposure, and service interruption.

### Remediation

1. Separate public provider configuration from resolved authentication material. Keep secret values in a private, non-reflected request configuration.
2. Introduce a credential-provider interface for environment, dedicated-server secret stores, or an application-supplied backend token source.
3. Deprecate serialized `ApiKeyOverride` safely. Preserve reflected-field compatibility long enough to migrate existing assets, but stop treating it as a recommended configuration path.
4. Return a redacted configuration from Blueprint and other inspection helpers. Never return API keys or sensitive additional headers.
5. Disable automatic `.env` loading in packaged game clients. Permit it for editor/development workflows and, when explicitly configured, dedicated servers.
6. Add configurable HTTPS and host allow-list enforcement for hosted profiles. Preserve an explicit development opt-out for trusted local endpoints.
7. Add validators and automation tests that inspect packaged configuration and reflected Blueprint surfaces for credential-bearing fields.

### Acceptance criteria

- A Shipping client package contains no provider key or sensitive header when built from supported configuration paths.
- Blueprint code cannot read resolved provider credentials.
- Direct hosted-provider authentication from a non-server Shipping target is rejected by default or requires a conspicuous, documented opt-in.
- Dedicated-server credentials can be supplied without storing them in plugin or project assets.
- Migration guidance exists for projects that previously used `ApiKeyOverride`.
- Logs, error objects, retry events, and validation artifacts are verified not to expose authorization values.

## P1: Gate releases on native Unreal validation

### Current state

Portable validation runs on pushes and pull requests. The native Unreal Engine workflow defines macOS, Windows, and Linux jobs, but it is manually dispatched and is not a required release gate. Release automation can publish a version without consuming evidence from the native matrix.

The macOS strict package build has compiled the Editor, Development Game, and Shipping Game targets. The test host must explicitly disable unavailable engine plugins such as `AndroidFileServer` so that automation starts consistently in installed-engine environments. With that host correction applied during review, all 11 current `UnrealAI.*` automation tests passed on macOS. Equivalent Windows and Linux evidence has not yet been recorded.

### Production risk

Portable source checks cannot detect Unreal Header Tool failures, compiler differences, reflection breakage, packaging problems, or host-specific HTTP behavior. A release tag may therefore represent code that has never compiled on one or more advertised platforms.

### Remediation

1. Fix and validate the minimal automation host so it enables only the plugins required by UnrealAI.
2. Run strict plugin packaging and automation on isolated or ephemeral macOS, Windows, and Linux Unreal runners for trusted commits.
3. Require portable validation for pull requests and protect `main` from unchecked merges.
4. Record a native qualification result for the exact release commit. Do not run untrusted public pull-request code on a persistent privileged self-hosted runner.
5. Make release publication depend on successful native evidence for every platform in the declared support matrix.
6. Retain packaged plugins, logs, and machine-readable test reports as build artifacts for an appropriate audit period.
7. Add scheduled validation so runner drift and Unreal toolchain changes are detected even when the source is unchanged.

### Acceptance criteria

- The automation host launches from a clean installed-engine environment on every supported host.
- The exact release commit passes `BuildPlugin -Rocket -StrictIncludes` and all `UnrealAI.*` tests on macOS, Windows, and Linux.
- Repository rules prevent merging when required portable checks fail.
- A release cannot be published without successful native qualification for its commit.
- Native reports identify the Unreal Engine version, host architecture, compiler/toolchain, target configurations, and test counts.

See [Continuous integration](ContinuousIntegration.md) for the current workflow and runner model.

## P1: Add runtime resource and latency budgets

### Current state

Streaming bounds an individual SSE event to 1 MiB and the cross-thread pending queue to 4 MiB. These safeguards prevent an unread network queue or incomplete event from growing indefinitely.

The complete accumulated response, one-shot response body, and number of simultaneous requests remain unbounded by the SDK. A stream drains all currently queued chunks in one game-thread task. Streaming uses an activity timeout, but not a total wall-clock deadline; a peer that periodically sends data can keep the operation alive indefinitely.

### Production risk

Provider mistakes, hostile compatible endpoints, long generations, network bursts, or accidental request fan-out can consume excessive memory and game-thread time. Even bounded network input can cause a visible frame hitch when all queued JSON and events are parsed in one frame.

### Remediation

1. Add provider and per-request limits for one-shot body bytes, total streamed bytes, accumulated text, output events, and active requests.
2. Enforce `Content-Length` early when present and retain an incremental byte limit when it is absent or incorrect.
3. Add an absolute request deadline independent of the existing per-attempt or activity timeout.
4. Drain streaming work through a configurable per-tick byte, event, or time budget while preserving event order and exactly-once terminal delivery.
5. Define overflow outcomes with stable error codes and retain only a documented amount of partial response data.
6. Add concurrency admission control and expose queued, rejected, cancelled, and active states to application telemetry.
7. Stress test large Unicode events, long streams, burst delivery, cancellation during draining, and many simultaneous requests.

### Acceptance criteria

- Every request has finite configurable limits for response size and total elapsed time.
- The SDK has a documented maximum active-request policy.
- Streaming parsing cannot monopolize an entire game frame under the configured budget.
- Exceeding any limit produces one deterministic terminal result and releases transport/delegate state.
- Stress tests demonstrate bounded memory and stable frame-time behavior under the documented workload.

## P1: Support current provider response APIs

### Current state

The additive [Responses API](Responses.md) now normalizes text/refusal parts, function calls/results, structured-output requests, item-level streaming, and provider-bound continuation across native OpenAI Responses, compatible Chat Completions/xAI, Anthropic Messages, and Gemini generateContent. Existing chat methods and Blueprint assets retain their contracts.

Local history preserves encrypted reasoning and signed provider blocks. Native OpenAI stored continuation is opt-in. The core SDK does not execute tools, orchestrate an agent loop, or translate histories between providers.

### Remaining production risk

Media, hosted tools, Gemini Interactions, background response management, and typed representations for every reasoning/citation/grounding field remain outside the normalized contract. Those fields are preserved as opaque provider data. Adapter capabilities do not guarantee support by every model or compatible endpoint, and schema validation remains provider/application-specific.

### Remaining remediation

1. Add separately scoped media/upload, embedding, hosted-tool, and background APIs when required by real consumers.
2. Evaluate Gemini Interactions independently; preserve the generateContent adapter and existing compatibility paths.
3. Add typed metadata only where semantics align across providers, retaining unknown raw fields.
4. Maintain provider/version fixtures and optional live acceptance tests to detect provider drift.
5. Keep tool authorization, schema validation, step budgets, durable state, and execution idempotency in the trusted application/backend.

### Acceptance and evidence

The native contract runner compiles C++ examples, generates and reloads actual Blueprint response graphs, and exercises two-step tool round trips over all four wire protocols through a credential-free local HTTP fixture. It tests request validation, typed tool assembly, signatures, local/stored continuation construction, streaming, incomplete output, and legacy compatibility. This addresses the initial text/tool abstraction milestone, not the deferred capabilities above or certification of live models.

## P2: Add end-to-end HTTP lifecycle tests

### Current state

Offline automation exercises serialization, provider fixtures, SSE parsing, queue limits, retry classification, retry coordination, Blueprint reflection, and helpers. Responses sample contracts now drive actual `FHttpModule` requests through a deterministic loopback HTTP endpoint using both C++ and generated Blueprint nodes across all four protocols. They cover tool continuation, split delivery, Unicode, retry success, early EOF/no replay, cancellation, and game-thread terminal delivery. This does not yet cover every timeout, destruction, or race scenario.

### Production risk

Parser and coordinator tests can pass while regressions remain in status-code callback ordering, partial network delivery, connection closure, cancellation, timeouts, retries, delegate cleanup, and game-thread dispatch.

### Remediation

- Add an injectable transport and scheduler boundary, or an isolated deterministic loopback HTTP server available only to tests.
- Cover one-shot success/error, split SSE frames, UTF-8 boundaries, malformed content types, early EOF, delayed headers, cancellation, activity timeout, absolute deadline, retry success, retry exhaustion, and cancellation during backoff.
- Assert one terminal callback, released request/delegate ownership, preserved event ordering, stable logical request identity, and zero callbacks after destruction.
- Keep provider-credential live tests opt-in and separate from the deterministic release gate.

### Acceptance criteria

- The real client entry points and HTTP callbacks are exercised in automation.
- Lifecycle tests are deterministic, offline, credential-free, and run on each supported native host.
- Race-sensitive tests can control transport and scheduler progress without wall-clock sleeps.

## P2: Normalize safety, refusal, and empty outcomes

### Current state

Provider HTTP errors are normalized, but successful responses can legitimately contain no visible text. The Responses API distinguishes incomplete generations and exposes explicit OpenAI refusals and supported Gemini block outcomes as refusal parts. The legacy chat contract remains unchanged. Detailed safety ratings, grounding, and some provider-specific finish categories still require raw metadata or finish-reason handling.

### Production risk

Gameplay cannot reliably distinguish “the model returned no text,” “the request was refused,” “content was blocked by policy,” and “the adapter did not understand the response.” Treating these states as interchangeable produces confusing user interfaces and incomplete safety handling.

### Remediation

- Add normalized outcome and finish categories for completed, length-limited, refused, safety-blocked, filtered, cancelled, and failed operations.
- Preserve provider reason codes and raw metadata as optional sensitive diagnostics.
- Define whether a refusal or safety block is a successful transport result, a distinct terminal result, or both; apply the same contract to one-shot and streaming calls.
- Provide Blueprint branches or helpers that encourage explicit handling of every terminal outcome.

### Acceptance criteria

- Fixture tests cover refusal, safety blocking, empty candidates, filtered output, and mixed text-plus-metadata responses for every provider.
- C++ and Blueprint consumers can handle these outcomes without parsing provider JSON.
- Documentation defines which outcomes are errors and which are valid model results.

## P2: Provide a native provider extension interface

### Current state

Custom OpenAI-compatible profiles are supported. The native adapters for other wire protocols are selected internally, so adding a new protocol requires modifying the UnrealAI module.

### Production risk

Studios using an internal gateway or a provider with a different schema must maintain a fork. That increases upgrade cost and discourages ecosystem integrations.

### Remediation

- Expose a versioned adapter interface or Unreal modular-feature registry for validation, request construction, one-shot parsing, and stream parsing.
- Define ownership, thread, module-loading, error, and capability-discovery contracts.
- Keep credential resolution separate from the adapter so third-party providers cannot accidentally receive unrelated secrets.
- Add a small test adapter in automation to prove registration, use, unload, and duplicate-name behavior.

### Acceptance criteria

- A separate runtime plugin can register and use a provider without changing UnrealAI source.
- C++ and Blueprint callers can select the provider through the existing high-level client flow.
- ABI/API version mismatches fail clearly, and adapter unload cannot leave an active request calling unloaded code.

## P2: Build a supported compatibility matrix

### Current state

Unreal Engine 5.7 is the currently validated release. The code uses Unreal platform abstractions and avoids known macOS-only runtime paths, but source portability is not the same as compiled support. Current native evidence is limited to macOS; dedicated-server, Windows, Linux, mobile, and console targets have not all been qualified.

### Production risk

Studios cannot infer platform support from a runtime-module declaration. Unreal HTTP backends, toolchains, packaging rules, certificates, and platform policies differ, especially on consoles and restricted platforms.

### Remediation

1. Publish an engine-version, host, target, and architecture matrix with one of `Supported`, `Experimental`, or `Not tested` for each entry.
2. Add a dedicated-server consumer target and smoke test to the sample/host projects.
3. Qualify Editor, Development, and Shipping configurations separately where behavior or configuration differs.
4. Test the previous supported Unreal minor version if the project promises more than a single-version support window.
5. Treat mobile and console support as explicit workstreams that include platform-holder requirements and real-device evidence.
6. Document the game-thread-only portion of the public client contract and add development assertions where misuse could race mutable state.

### Acceptance criteria

- Every public support claim links to a successful repeatable build/test job for the relevant host and target.
- A Shipping dedicated-server target compiles and completes one-shot and streaming loopback tests.
- Unsupported platforms fail clearly or are omitted from the descriptor/support matrix rather than being implied.

## P2: Add production observability and fleet controls

### Current state

Callers receive normalized errors, usage when providers report it, retry notifications, and logical request handles. There is no first-class instrumentation contract for latency phases, provider request IDs, rate-limit headers, queue pressure, aggregate usage, or circuit state.

### Production risk

Operators need to diagnose provider degradation, protect budgets, correlate support incidents, and prevent large server fleets from retrying simultaneously. Logging prompts or raw responses to obtain that visibility creates a privacy risk.

### Remediation

- Add opt-in structured telemetry events for request start/end, attempts, latency, retry delay, bytes, token usage, limits, and normalized outcome.
- Capture provider request IDs and rate-limit metadata in redacted typed fields.
- Never include authorization headers, prompt content, response content, or raw JSON in default telemetry.
- Add hooks for application-level admission control, per-player quotas, budgets, and circuit breakers.
- Review POST retry semantics and provider idempotency support so ambiguous transport failures do not silently duplicate costly work.
- Prefer a documented fleet-safe jitter strategy and honor bounded provider retry guidance.

### Acceptance criteria

- A server can calculate success rate, p50/p95/p99 latency, retry rate, provider error rate, usage, and limit rejections without collecting model content.
- Sensitive-data redaction has automated tests.
- Integrators can disable, sample, or route telemetry without changing the client implementation.
- Operational guidance defines recommended concurrency, timeout, retry, quota, and circuit-breaker defaults.

## P2: Publish versioned artifacts and a support policy

### Current state

Semantic versions, tags, changelogs, and GitHub Releases are automated. Releases do not yet provide qualified packaged-plugin artifacts, checksums, a compatibility manifest, populated support/documentation metadata, or a formal deprecation window.

### Production risk

Studios must build directly from source and determine compatibility themselves. Without upgrade and support expectations, adopting a pre-`1.0` reflected API creates additional risk for serialized Blueprint assets and long-lived game branches.

### Remediation

- Attach CI-produced plugin packages and checksums to each qualified GitHub Release.
- Identify the Unreal version, target platforms, source commit, and toolchains used for each artifact.
- Populate `DocsURL` and `SupportURL` in `UnrealAI.uplugin` when stable destinations exist.
- Define supported Unreal versions, security-reporting instructions, response expectations, and end-of-support rules.
- Document reflected API migrations and retain Unreal redirects or deprecated wrappers when Blueprint assets would otherwise break.
- Define the stability guarantees required before declaring `1.0.0`.

### Acceptance criteria

- Users can download a versioned artifact whose checksum and source commit are published.
- Installation, supported engines/platforms, known limitations, and migration notes are present for every release.
- Public API removals follow the documented SemVer and deprecation policy.
- A private security-reporting path and a public support path are documented.

## Application-level launch checklist

Even after the SDK gaps above are complete, each game owns deployment and product policy. Before launch, the integrating game should confirm:

- hosted-provider credentials exist only on a trusted backend or dedicated server;
- player identity and authorization are checked before consuming provider quota;
- prompts and model output are treated as untrusted data and cannot directly execute privileged gameplay actions;
- tool calls are allow-listed, schema-validated, authorized, bounded, and audited by the game;
- input/output moderation and age-appropriate experiences match the title, audience, storefront, and jurisdiction;
- privacy disclosures cover provider processing, retention, telemetry, and cross-region data flow;
- provider outages have a defined fallback, degraded mode, and player-facing message;
- per-player and fleet-wide rate, token, cost, and concurrency budgets are enforced;
- logs and crash reports redact prompts, responses, raw JSON, credentials, and personal data by default;
- cancellation, map travel, disconnect, shutdown, hot reload, and object destruction cannot leave work or callbacks behind;
- model and prompt changes are versioned, evaluated, staged, and reversible independently of a game binary release.

## Readiness review cadence

Review this document before every minor release and before expanding the declared platform or provider matrix. For a `1.0.0` readiness decision:

1. close all P1 gaps;
2. either close each P2 gap or explicitly document the supported boundary and owner;
3. attach native qualification evidence for the release commit;
4. complete a security review of credential flow, configuration serialization, logging, and compatible-endpoint behavior;
5. run load, soak, cancellation, and fault-injection tests representative of the intended game workload;
6. verify the public C++ and Blueprint migration policy against a real consuming project.

Production readiness is a maintained property, not a one-time milestone. Provider APIs, model behavior, Unreal Engine versions, platform policies, and operating requirements will continue to change.
