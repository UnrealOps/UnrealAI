# Response Async Nodes and Bounded Tool Graphs

Use this reference for typed function tools, structured output, or continuation in Blueprint. Keep the legacy chat component/async recipes for existing text-chat flows.

## Inspect and select

Read `Source/UnrealAI/Public/UnrealAIResponseTypes.h`, `UnrealAIResponseLibrary.h`, and `UnrealAIResponseAsyncAction.h`. From the consuming plugin root, read `Documentation/Responses.md` for provider capabilities, protocol selection, storage, and status semantics.

Use `Create Response (UnrealAI)` or `Stream Response (UnrealAI)`, not the legacy Chat Completion nodes. Select the provider with `Provider Name`. Handle `Completed`, `Incomplete`, `Failed`, and `Cancelled` separately, and bind `Retrying`. For streams, switch on `ResponseEvent.Type` from `Event`; only `TextDelta` belongs in visible incremental text. Retain `Async Action` for `Cancel`.

All execution paths share one delegate signature so the standard K2 async node exposes `Result`, `ResponseEvent`, and `RetryEvent` data. Read only the data matching the active execution pin; the other values are empty defaults. The proxy pin's internal name is `AsyncTaskProxy`, not its friendly label.

## Tool round trip

1. Construct `UnrealAIResponseRequest` / `Make Response Request`. Set `Instructions` separately from user input. `Make Tool Definition` takes a JSON object schema.
2. After terminal `Completed`, use `Get Response Tool Calls`. An item-completed event or partial argument delta is not permission to execute a tool.
3. Validate allowed name, argument schema, gameplay permissions, and remaining step budget in trusted application code.
4. Return one `Make Tool Result` for each pending call, then branch on `Build Continuation Request` success.
5. Submit the returned request. Do not append a second copy of the previous assistant message or discard opaque history/signatures.
6. Bound total model/tool steps and keep separate histories per conversation. Cancellation/incomplete/failure does not commit continuation history.

A pure Blueprint owner may use the helpers directly. For mixed integration, choose exactly one history/tool owner. Never expose provider keys, raw history, raw errors, or reasoning metadata to an untrusted packaged client.

## Generate and compile real assets

Read [asset-workflow.md](asset-workflow.md) before modifying binary assets. The canonical editor implementation is `Samples/UnrealAISample/Source/UnrealAISampleEditor/Private/UnrealAIResponseExampleGenerator.cpp`, resolved from the plugin root.

The generator saves `/Game/Blueprints/BP_UnrealAIResponses` and `BP_UnrealAIStreamResponses`. Each derives from `AUnrealAIResponseExample` and overrides `StartBlueprintResponses`:

- Prepare Response Request → Branch → first response async node.
- Completed → Prepare Tool Continuation → Branch → second response async node.
- Second Completed and all non-completed terminals → Record Response Terminal.
- Each Event / Retrying / Async Action output → its matching record/retention function.

The base class validates and executes one read-only `get_level_name` call; the graph owns both asynchronous submissions. This tests actual nodes and Blueprint bytecode, not only native delegate handlers. Do not add an automatic BeginPlay request or a key pin. For a shipped client, use a trusted server/backend facade instead.

Run `Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>`. It compiles the native base, regenerates both assets, checks their resolved terminal pins, reloads them, and executes both two-step Blueprint flows over loopback HTTP for all four protocols. Require `UnrealAISample.SkillContracts.Responses.BlueprintAssets` and `UnrealAISample.SkillContracts.Responses.Loopback`, plus `UnrealAI.Responses.*`. Report the actual platform and any provider/live-test limitations.
