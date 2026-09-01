# UnrealAI

UnrealAI is a provider-neutral Unreal Engine runtime plugin for OpenAI-compatible chat generation APIs.

- [Plugin documentation](Documentation/README.md)
- [Continuous integration](Documentation/ContinuousIntegration.md)

## Quick validation

Run the portable repository checks without installing Unreal Engine:

```bash
python3 Scripts/ci/validate_plugin.py
```

With a macOS Unreal Engine installation available, package the plugin and run its native automation tests:

```bash
UNREAL_ENGINE_ROOT=/path/to/UnrealEngine Scripts/ci/run-unreal-ci.sh
```
