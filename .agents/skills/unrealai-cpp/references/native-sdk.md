# Native SDK integration

Use `FUnrealAIExecutionService` when the existing convenience API is needed without a UObject owner. Own it with a thread-safe shared pointer, configure before use, call control methods on the game thread, and call `Shutdown` before retiring the owner. Preflight failures may complete synchronously. Existing factory-created `UUnrealAIClient` instances forward to this service.

Use `IUnrealAIModelProvider` for agent runtimes. Include the defining headers from `Models/`, configure exact connections and model profiles, inject credentials and transport, and enqueue worker callbacks into the caller's owning thread. It executes one model turn without retrying or executing tools. Only a completed turn may authorize tool execution or continuation. Keep the SDK loaded until all physical request handles and callbacks settle.

The base SDK contains UnrealAI, UnrealAIAccess, and UnrealAITransport. Add dependencies only for headers actually used. UnrealAIAuth supplies platform stores and OAuth; UnrealAIExperimentalAccess supplies restricted subscription integrations. Install addons as siblings with `Scripts/install_addons.py`; neither is a base dependency. Use an injected application broker when no addon is installed.

The native strict transport is currently implemented for Mac; other hosts report unsupported. The existing convenience transport remains portable. Explicit Gemini Interactions uses the native provider constructor; convenience Gemini continues using generateContent. Native request DTOs and streams are bounded, and absolute monotonic deadlines distinguish logical completion from physical settlement.

Native Responses endpoint policy defaults to sending `max_output_tokens`. Only an adapter for an endpoint that rejects it should set `bSendMaxOutputTokens = false`; the optional subscription adapter does this explicitly. Without the field, the provider controls generation length and the application still owns usage budgets. Keep public API and Chat Completions defaults unchanged. Native HTTP `UserMessage` includes status and recognized SDK-authored explanations; never replace it with raw provider error text.

Native Responses projects inline strict tool schemas without changing the original application schema: unsupported format annotations are omitted, and optional non-null properties use nullable wire fields. Completed calls remove only synthetic optional nulls. Calls needing this projection omit raw argument deltas; consume their completed arguments and validate against the original schema. Continuations preserve the provider's original arguments. Compound/reference schema resolution remains outside this projection.

The subscription endpoint policy explicitly allows an absent response media type through `bAllowMissingResponseContentType`. This does not accept an explicit non-SSE type or relax stream validation. Keep the default requirement for other endpoints unless their trusted adapter needs the exception.

Its `bAllowEmptyTerminalOutput` policy also accepts metadata-only successful terminals after complete streamed output items. It does not authorize partial calls, a changed non-empty output list, or a stream without a terminal. Preserve the original completed items in continuation history. Optional compound/reference tool schemas cannot be safely projected into nullable strict fields without a resolver and are rejected; callers can explicitly select `bStrict = false` and retain local validation.

For Shipping clients, use an explicit GatewayBearer/GatewayAccounted connection with a short-lived game credential, or the game's authenticated network service. The convenience ApiKeyOverride path is rejected in Shipping clients, including when a game session token is placed there. A boolean authority check does not establish a trusted credential store.

Full ownership, installation, API, and limits are described in `Documentation/NativeSDK.md` in the plugin checkout. Agent tools, scene perception, memory, budget charging, workflows, delegation, and movement belong to the consuming application.
