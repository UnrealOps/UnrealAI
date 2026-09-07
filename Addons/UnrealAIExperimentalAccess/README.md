# UnrealAIExperimentalAccess

Optional OpenAI/xAI subscription compatibility drivers and resource policies. These existing integrations remain restricted development features; no entitlement or support status changes are implied.

Install this directory as a sibling of the UnrealAI plugin and enable it explicitly in the project. UnrealAIExperimentalAccess also requires UnrealAIAuth. Its modules are excluded from Server and Shipping builds. The SDK source checkout provides `Scripts/install_addons.py` for source installation. Packaged addons can be extracted directly into the project’s `Plugins` directory.

See `Documentation/NativeSDK.md` in the UnrealAI checkout for native API selection, client/server boundaries, lifetime rules, and platform qualification. Ordinary Blueprint clients do not require this addon.
