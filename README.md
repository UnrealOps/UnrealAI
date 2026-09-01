# UnrealAI

UnrealAI is a provider-neutral Unreal Engine runtime plugin for OpenAI-compatible chat generation APIs.

- [Plugin documentation](Documentation/README.md)
- [Continuous integration](Documentation/ContinuousIntegration.md)

## Quick validation

Run the portable repository checks without installing Unreal Engine:

```bash
python3 Scripts/ci/validate_plugin.py
```

With a native Unreal Engine installation available, package the plugin and run its automation tests for the current host platform:

```bash
UNREAL_ENGINE_ROOT=/path/to/UnrealEngine python3 Scripts/ci/run_unreal_ci.py --platform Mac
```

Use `Win64` on Windows or `Linux` on Linux.
