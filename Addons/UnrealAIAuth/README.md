# UnrealAIAuth

Optional secure credential storage, API-key provisioning, browser/device OAuth, and account UI for UnrealAI.

Install this directory as a sibling of the UnrealAI plugin and enable it explicitly in the project. The SDK source checkout provides `Scripts/install_addons.py` for source installation. Packaged addons can be extracted directly into the project’s `Plugins` directory.

See `Documentation/NativeSDK.md` in the UnrealAI checkout for native API selection, client/server boundaries, lifetime rules, and platform qualification. Ordinary Blueprint clients do not require this addon.
