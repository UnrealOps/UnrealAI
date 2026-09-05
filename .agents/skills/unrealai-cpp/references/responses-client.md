# Native Responses and Tool Continuation

Use this reference for `CreateResponse`, `StreamResponse`, typed tools/structured output, or provider-bound continuation. Do not load the legacy streaming/multi-turn recipes for this integration. Read `client-setup.md` only if configuring a module or provider.

## Ground the contract

Read `Source/UnrealAI/Public/UnrealAIResponseTypes.h`, `UnrealAIResponseLibrary.h`, and the new methods in `UnrealAIClient.h`. From the consuming plugin root, read `Documentation/Responses.md` for the capability matrix and protocol semantics.

Use the complete `Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIResponseExample.h` and matching `Private/UnrealAIResponseExample.cpp`, resolved from the plugin root, not a shortened callback fragment. Replace the consumer module API macro when copying them. `StartNativeResponses(false)` and `StartNativeResponses(true)` demonstrate both transports against the same bounded tool flow.

## Required workflow

1. Retain/configure a `UUnrealAIClient`; retain the returned handle for cancellation. Respect synchronous preflight failure and game-thread callbacks.
2. Create `FUnrealAIResponseRequest` with `MakeResponseRequest`. Put system guidance in `Instructions`; text input messages are user/assistant roles.
3. Add function definitions and explicitly choose tools/output format if needed. Query adapter capabilities; validate request construction. Model-specific constraints remain provider-enforced.
4. On events, append only `TextDelta` to visible text. Keep argument deltas separate. Treat provider JSON and signatures as sensitive opaque data.
5. Handle all four terminal statuses. Only `Completed` permits `GetResponseToolCalls`; `Incomplete`, failure, cancellation, or `ItemCompleted` alone never authorizes a game action.
6. Allowlist tools, validate argument shape and application authorization, execute within a bounded step budget, and produce one `MakeToolResult` per pending call.
7. Use `BuildContinuationRequest` with the matching request/result pair. Never manually flatten signed/reasoning history or move it across provider/model boundaries.
8. Cancel active work at owner teardown. Keep independent histories/handles for different players; never let presentation code mutate the same history.

The SDK does not execute tools or manage an agent loop. The sample permits one read-only tool and two model requests total. It rejects client-mode execution, but a listen server is still untrusted for hosted-provider credentials. Choose the relevant deployment reference for a production trust boundary.

Official OpenAI uses native Responses by default only for the new methods. Custom compatible endpoints default to Chat Completions; `ResponseApi=OpenAIResponses` is an explicit opt-in. Legacy chat APIs remain separate and unchanged.

## Behavioral validation

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>` with `UNREAL_ENGINE_ROOT` configured. Require `UnrealAI.Responses.*` and `UnrealAISample.SkillContracts.Responses.Loopback`. The latter drives actual public native methods and HTTP callbacks through a credential-free loopback fixture over all four protocols, including streaming, continuation, retries, and cancellation. Report the actual host, tests, and any unavailable live-provider/platform validation.
