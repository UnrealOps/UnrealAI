# Blueprint Asset Generation and Compilation

Use this workflow only when the requested result is a real Blueprint asset. For an explanation, review, or node recipe, use `async-actions.md` or `chat-component.md` without modifying binary content.

The repository's canonical implementation is the `UnrealAISampleGenerate` commandlet in `Samples/UnrealAISample/Source/UnrealAISampleEditor/Private/UnrealAISampleGenerateCommandlet.cpp`. Its offline asset contracts are `UnrealAISample.SkillContracts.Blueprint.GeneratedAsset` and `UnrealAISample.SkillContracts.Blueprint.GeneratedBackendWidget`; the controller-backed widget behavior is covered separately by `UnrealAISample.SkillContracts.Blueprint.PackagedWidgetBehavior`.

## Safety boundary

- Never edit, patch, or synthesize `.uasset` bytes outside Unreal Editor.
- Do not overwrite an existing asset until checking repository status and confirming that regeneration will not discard user-authored graph changes.
- Prefer an isolated copy of the consuming project for the first generator and test run. Generated `Binaries`, `DerivedDataCache`, `Intermediate`, and `Saved` directories must remain untracked.
- A Markdown graph, screenshot, or node list is not proof of compilation. If the editor cannot run, provide an exact node-and-pin recipe and state that the asset remains uncompiled.

## Put generation code in an editor module

Create the commandlet or editor utility in a project or plugin editor module, never a runtime module. The sample module declares the dependencies needed by its implementation:

- `AssetRegistry`
- `BlueprintGraph`
- `Core`
- `CoreUObject`
- `Engine`
- `Kismet`
- `UMG` and `UMGEditor` when creating a `UWidgetBlueprint`
- the UnrealAI runtime module and the consuming project module
- `UnrealEd`

Keep the generated Blueprint dependent only on runtime-safe classes and functions. Editor module dependencies must not leak into the packaged runtime target.

## Generate the graph

1. Load the target package and asset if they exist. Otherwise call `CreatePackage`, then create the Blueprint with `FKismetEditorUtilities::CreateBlueprint` and notify the asset registry with `FAssetRegistryModule::AssetCreated`. A Widget Blueprint must use `UWidgetBlueprint::StaticClass()` and `UWidgetBlueprintGeneratedClass::StaticClass()`, not ordinary Blueprint classes.
2. Find the intended graph with `FBlueprintEditorUtils::FindEventGraph`. Preserve existing nodes unless the requested workflow explicitly owns the whole generated graph.
3. Resolve every callable from the live reflected surface. Use `GET_FUNCTION_NAME_CHECKED` with `FindFunctionByName` so a renamed C++ function breaks the editor build or generator instead of producing a stale node silently.
4. Create ordinary call nodes with `FGraphNodeCreator<UK2Node_CallFunction>` and `SetFromFunction`. Create UnrealAI async nodes with `FGraphNodeCreator<UK2Node_AsyncAction>` and `InitializeProxyFromFunction`.
5. Find each pin by its exact current name and direction. Treat a missing pin as a generator error and print the available pin names for diagnosis.
6. Validate a proposed default with `IsPinDefaultValid`, apply it through `UEdGraphSchema_K2::TrySetDefaultValue`, then require `IsCurrentPinDefaultValid` to return no error and confirm the pin retained the expected value. In Unreal Engine 5.7, `TrySetDefaultValue` returns `void`; do not invent a boolean result. Abort before saving when validation or the retained value fails. Never store an API key or other secret in a pin default.
7. Connect pins through `UEdGraphSchema_K2::TryCreateConnection`. Abort on a rejected connection; do not save a partially wired graph.
8. Include every required lifecycle route. One-shot graphs normally connect `Completed`, `Retrying`, `Failed`, and `Cancelled`; direct streaming graphs connect `Event`, `Retrying`, `Completed`, `Failed`, and `Cancelled`. The generated packaged-chat widget instead provides layout-free entry points for `ReceiveBackendReadinessChanged`, `ReceiveTextDelta`, `ReceiveRetrying`, `ReceiveCompleted`, `ReceiveFailed`, and `ReceiveCancelled` from its native controller. The consuming game must connect those entries to its own widget tree; repository behavior proof lives in the native controller contract. Use `Get First Choice Content` and consume `Has Content` before accepting a direct SDK response.
9. Call `FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified` after the graph is complete.

Retain the async node's `Async Action` output in generated graphs that need cancellation. Do not log `Provider Event`, raw provider bodies, prompts, or secrets by default.

## Compile and save

Compile before writing the package:

```cpp
FKismetEditorUtilities::CompileBlueprint(Blueprint);
if (Blueprint->Status == BS_Error)
{
	return 1;
}
```

Only after a successful compile, mark and fully load the package, construct `FSavePackageArgs`, derive the filename with `FPackageName::LongPackageNameToFilename`, and call `UPackage::SavePackage`. Return a nonzero commandlet exit code for graph construction, compilation, or save failures.

If the workflow also creates a map, compile the Blueprint before reading `GeneratedClass`, place exactly the intended actor instances, mark the map package dirty, and save it independently.

## Run the repository sample workflow

Use the command-line editor executable supplied by the installed Unreal Engine. Its filename is `UnrealEditor-Cmd.exe` on Windows and `UnrealEditor-Cmd` on macOS and Linux.

Generate and compile the sample asset:

```text
<UnrealEditor-Cmd> <path-to>/UnrealAISample.uproject -run=UnrealAISampleGenerate -unattended -nop4 -NullRHI
```

Then regenerate a fresh isolated copy, reload it, and validate the saved graph without provider credentials:

```text
<python3> Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>
```

Run each command with its native host executable and toolchain. Do not translate paths or shell syntax from one operating system into a hard-coded repository instruction.

## Write an asset-contract test

The test must validate behavior-bearing structure rather than merely checking that a file exists:

1. Load the Blueprint object by package path.
2. Call `FKismetEditorUtilities::CompileBlueprint` again and assert that its status is not `BS_Error` and `GeneratedClass` exists.
3. Walk the Blueprint graphs and identify async nodes by their exact factory `UFunction`, not by localized display text or node coordinates.
4. Assert that every required execution output has at least one link. For the sample one-shot graph these are `Completed`, `Retrying`, `Failed`, and `Cancelled`. For a controller-backed Widget Blueprint, identify all required override-event nodes by their exact reflected member reference and separately run the controller behavior contract.
5. Inspect important data connections and defaults when they define the example's contract. The sample contract checks its request connection, provider profile default, prompt default, response-helper input, content output, and consumed `Has Content` output.
6. If a map is generated, load it and confirm the expected generated class is placed exactly as intended. If a Widget Blueprint is generated, assert that it inherits the intended credential-free runtime controller and instantiate the generated class in a behavior test.

Keep this test offline. Network success belongs in a separate opt-in live test and must receive credentials through the process environment only.

## Completion checklist

- The native editor target builds on the current host.
- The contract runner creates an isolated project copy, executes the generator there, and requires a zero exit code before automation starts.
- The generated Blueprint and `WBP_UnrealAIBackendChat` reload and compile without errors.
- The asset-contract automation tests cover expected nodes, pins, defaults, and widget event entry points, while the separate controller behavior contract covers terminal routing.
- `Scripts/ci/validate_skills.py` and `Scripts/ci/validate_plugin.py` pass from the plugin root, and the native skill-contract report contains the required generated-asset test.
- Repository status contains only intended source, documentation, and explicitly requested asset changes; no environment files or generated output are staged.
- Report which host and Unreal Engine version were actually exercised. Do not infer cross-platform compilation from one host run.
