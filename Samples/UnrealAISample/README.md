# UnrealAI Sample Project

## Typed Responses examples

`AUnrealAIResponseExample` demonstrates native one-shot and streaming requests, an allowlisted read-only `get_level_name` tool, and provider-bound continuation capped at two model requests. Call `StartNativeResponses(false)` or `StartNativeResponses(true)` on a trusted standalone/server actor.

The existing `UnrealAISampleGenerate` commandlet also generates and compiles `BP_UnrealAIResponses` and `BP_UnrealAIStreamResponses`. They override `StartBlueprintResponses` with two real response async nodes and use the native base for tool validation/history ownership. They do not start network requests in BeginPlay.

Read the [Responses guide](../../Documentation/Responses.md) for protocol selection, every terminal/event path, safe tool execution, and local versus stored continuation. `Scripts/ci/run_skill_contracts.py` builds an isolated sample, regenerates these assets, and tests all four protocols through a credential-free loopback fixture; it does not alter the checkout's binary assets.

## Existing chat examples

This Unreal Engine 5.7 project is a real consumer of the repository's `UnrealAI` plugin. It includes:

- `AUnrealAISampleActor`, a C++ example for one-shot completions, SSE streaming, retry notifications, cancellation, and safe response handling.
- `AUnrealAIMultiTurnExample`, a combined one-shot/streaming conversation owner with committed-only history, partial-result rollback, concurrency rejection, normalized text deltas, and local SDK lifecycle events.
- `UUnrealAIPackagedChatWidgetExample` plus generated `WBP_UnrealAIBackendChat`, a credential-free packaged-client Widget Blueprint boundary with compiled lifecycle behavior.
- `UUnrealAIPackagedClientFacadeExample`, a compile-tested native boundary that lets packaged Blueprint UI call an authenticated compatible game backend without reflected credential or provider controls.
- `BP_UnrealAIGettingStarted`, a compiled Blueprint that uses `Make Simple Chat Request`, `Create Chat Completion (UnrealAI)`, and `Get First Choice Content` directly.
- `UnrealAISampleMap`, the startup level with the Blueprint example already placed. The request starts on Begin Play.
- Offline compiled and behavioral skill contracts plus opt-in live C++ completion, C++ streaming, and Blueprint tests.

The `.uproject` discovers the adjacent repository plugin through a relative `AdditionalPluginDirectories` entry. No symlink, copied plugin, or machine-specific path is required.

## Run the sample

1. Copy the repository environment template into this directory.

   macOS or Linux:

   ```bash
   cp ../../.env.example .env
   ```

   Windows PowerShell:

   ```powershell
   Copy-Item ..\..\.env.example .env
   ```

2. Add a key for the provider you want to exercise. The sample defaults to XAI and the plugin's configured `grok-4.6` model.

   ```dotenv
   XAI_API_KEY=your_api_key_here
   ```

3. Open `UnrealAISample.uproject` in Unreal Engine 5.7, allow Unreal to build the C++ modules, and press Play in `UnrealAISampleMap`.

4. Read the terminal status and response on the `UnrealAI Blueprint Getting Started` actor. Provider response text is kept on the actor and is not written to the log.

The `.env` file and all Unreal-generated directories are ignored by the repository. Never commit a real provider key. A shipped game should call a trusted backend instead of embedding a hosted-provider secret in the client.

## C++ example

The sample module declares `UnrealAI` as an explicit dependency in `Source/UnrealAISample/UnrealAISample.Build.cs`. The implementation in `Source/UnrealAISample/Private/UnrealAISampleActor.cpp` demonstrates the supported ownership and request flow:

```cpp
FUnrealAIError ConfigurationError;
UnrealAIClient = UUnrealAIProviders::XAI(this, ConfigurationError);
if (!UnrealAIClient)
{
	UE_LOG(LogTemp, Error, TEXT("Configuration failed: %s"), *ConfigurationError.Message);
	return;
}

const FUnrealAIChatRequest Request =
	UUnrealAIBlueprintLibrary::MakeSimpleChatRequest(Prompt);

ActiveCompletion = UnrealAIClient->CreateChatCompletion(
	Request,
	FUnrealAIChatCompletionNativeDelegate::CreateUObject(
		this,
		&ThisClass::HandleCompletion),
	FUnrealAIRetryNativeDelegate::CreateUObject(
		this,
		&ThisClass::HandleRetry));
```

The client is a `UPROPERTY`, and the returned request handles are retained for cancellation. `HandleCompletion` checks `FUnrealAIError` before reading the response and uses `GetFirstChoiceContent` so an empty choices array is safe. `RunCppStreamingSample` uses the dedicated `StreamChatCompletion` API and accumulates only normalized text-delta events.

To try the native path in the editor, select the placed actor and call `Run Cpp Completion Sample` or `Run Cpp Streaming Sample` from Blueprint, a Level Blueprint, or another gameplay class.

## Blueprint example

Open `Content/Blueprints/BP_UnrealAIGettingStarted` to inspect the direct-provider getting-started graph. It intentionally exposes every terminal or progress path:

- `Completed` passes the response through `Get First Choice Content`.
- `Retrying` records transient retry progress.
- `Failed` preserves and handles the `UnrealAIError`.
- `Cancelled` is handled separately from failure.

The asset calls the plugin's async Blueprint node directly; its C++ parent only stores the visible outcome so automation can verify the graph. Change the `Provider Name` pin from `XAI` to `OpenAI`, `Anthropic`, or `Gemini` and set the corresponding environment key to try another built-in profile.

For a shipped-client boundary, first run the generator below in a working or isolated sample copy, then open `Content/Blueprints/WBP_UnrealAIBackendChat`. The generated Widget Blueprint is deliberately not checked in. It inherits `UUnrealAIPackagedChatWidgetExample`; native session code injects an authenticated backend facade, while Blueprint implements six presentation/readiness event entry points without seeing a token, provider configuration, raw error, or provider event. Its controller keeps completed history separate from the pending turn and handles stale IDs, retry progress, cancellation, session-refresh readiness, and interrupted partial text. The generated asset is a verified layout-free skeleton—add the consuming game's controls, styling, localization, bounded transcript policy, and presentation wiring.

## Validate the sample

Both Blueprints and the map can be generated reproducibly from the editor module:

```text
<UnrealEditor-Cmd> UnrealAISample.uproject -run=UnrealAISampleGenerate -unattended -nop4 -NullRHI
```

Stage an isolated sample copy, compile it, regenerate its Blueprint/map, and run every credential-free C++, Blueprint, mixed, and deployment recipe contract:

```text
<python3> ../../Scripts/ci/run_skill_contracts.py --platform <Mac|Win64|Linux>
```

The required test names live in `../../Scripts/ci/skill_contracts.json`. The runner leaves this checkout's assets untouched and fails if generation fails or a named test is absent, skipped, incomplete, or unsuccessful; a matching source-code string is not considered evidence that the contract ran.

Live tests are deliberately excluded from normal repository CI. With `XAI_API_KEY` present only in the process environment, run them individually or select the `UnrealAISample.Live.XAI` prefix:

```text
Automation SetFilter Stress; RunTests UnrealAISample.Live.XAI.CppCompletion
Automation SetFilter Stress; RunTests UnrealAISample.Live.XAI.CppStreaming
Automation SetFilter Stress; RunTests UnrealAISample.Live.XAI.BlueprintCompletion
```

Native builds must run on their matching host toolchain: `Mac` on macOS, `Win64` on Windows, and `Linux` on Linux.
