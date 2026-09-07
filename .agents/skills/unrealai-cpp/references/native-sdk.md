# Native SDK integration

Use `FUnrealAIExecutionService` when the existing convenience API is needed without a UObject owner. Own it with a thread-safe shared pointer, configure before use, call control methods on the game thread, and call `Shutdown` before retiring the owner. Preflight failures may complete synchronously. Existing factory-created `UUnrealAIClient` instances forward to this service.

Use `IUnrealAIModelProvider` for agent runtimes. Include the defining headers from `Models/`, configure exact connections and model profiles, inject credentials and transport, and enqueue worker callbacks into the caller's owning thread. It executes one model turn without retrying or executing tools. Only a completed turn may authorize tool execution or continuation. Keep the SDK loaded until all physical request handles and callbacks settle.

The base SDK contains UnrealAI, UnrealAIAccess, and UnrealAITransport. Add dependencies only for headers actually used. UnrealAIAuth supplies platform stores and OAuth; UnrealAIExperimentalAccess supplies restricted subscription integrations. Install addons as siblings with `Scripts/install_addons.py`; neither is a base dependency. Use an injected application broker when no addon is installed.

The native strict transport is currently implemented for Mac; other hosts report unsupported. The existing convenience transport remains portable. Explicit Gemini Interactions uses the native provider constructor; convenience Gemini continues using generateContent. Native request DTOs and streams are bounded, and absolute monotonic deadlines distinguish logical completion from physical settlement.

For Shipping clients, use an explicit GatewayBearer/GatewayAccounted connection with a short-lived game credential, or the game's authenticated network service. The convenience ApiKeyOverride path is rejected in Shipping clients, including when a game session token is placed there. A boolean authority check does not establish a trusted credential store.

Full ownership, installation, API, and limits are described in `Documentation/NativeSDK.md` in the plugin checkout. Agent tools, scene perception, memory, budget charging, workflows, delegation, and movement belong to AutonomousAgents or another consumer.
