# UnrealAI Repository Instructions

This file applies to the entire repository. UnrealAI is a standalone Unreal Engine runtime plugin that provides provider-neutral OpenAI-compatible, Anthropic, and Google Gemini APIs to C++ and Blueprints. Keep the plugin usable when copied or cloned into an Unreal project's `Plugins/UnrealAI` directory.

## Project Baseline

- `UnrealAI.uplugin` is the plugin descriptor and version source of truth.
- The initial release is `0.1.0`; release tags use the `v0.1.0` form.
- Unreal Engine 5.7 is the currently validated engine release.
- `UnrealAI` is a runtime module. Do not introduce editor-only dependencies into its runtime or public API.
- Chat generation is currently non-streaming. Do not describe planned streaming or other roadmap features as implemented.
- The built-in provider defaults are defined in `Source/UnrealAI/Private/UnrealAISettings.cpp`. Avoid duplicating those values unless a user-facing example requires them, and update every documented and validated copy when they change.

## Repository Map

| Path | Responsibility |
| --- | --- |
| `UnrealAI.uplugin` | Plugin identity, modules, and release version |
| `Source/UnrealAI/Public/` | Exported C++ and Blueprint contracts |
| `Source/UnrealAI/Private/` | HTTP client, settings, serialization, async actions, and component behavior |
| `Source/UnrealAI/Private/Tests/` | Offline Unreal Automation tests |
| `Documentation/` | Detailed usage and CI documentation |
| `.agents/skills/` | Source-grounded C++ and Blueprint instructions for coding agents |
| `Scripts/ci/` | Portable validation, native packaging, and automation-report checks |
| `Tests/HostProject/` | Minimal project used to load and test the packaged plugin |
| `.github/workflows/` | Hosted source checks, release automation, and native self-hosted runner matrix |
| `release-please-config.json` | Conventional Commit release, changelog, and tag policy |
| `.release-please-manifest.json` | Automation-owned record of the last released version |
| `version.txt` | Automation-owned SemVer mirror used by the release strategy |
| `CHANGELOG.md` | Automation-owned release notes shipped with the plugin |

Never edit or commit generated `Binaries/`, `DerivedDataCache/`, `Intermediate/`, `Saved/`, Python bytecode, IDE output, or native CI package output.

## Working Agreement

1. Read `git status` before making changes and preserve unrelated user work.
2. Inspect the public header and implementation that own the behavior. Do not infer the API from README examples alone.
3. Make the smallest coherent change and update all affected C++, Blueprint, documentation, skill, test, and CI surfaces together.
4. Keep automated tests deterministic, offline, and independent of provider credentials.
5. Run validation proportional to the change and report the exact native host platform exercised.
6. Review the final diff for generated output, personal paths, secrets, stale identifiers, and accidental scope expansion.
7. Do not create releases, tags, or history-rewriting commits manually unless explicitly requested. Let the release workflow own normal tags and GitHub Releases.

When working in a consuming Unreal project, use `.agents/skills/unrealai-cpp` for native integrations and `.agents/skills/unrealai-blueprints` for Blueprint flows. The current checkout's public headers remain authoritative.

## Architecture and API Rules

- Keep provider-neutral types in the shared API. Provider-specific fields belong in configuration, raw JSON extension points, or clearly isolated provider adapters.
- Add public contracts under `Source/UnrealAI/Public/` and implementation details under `Private/`.
- Treat changes to exported structs, enums, delegates, `UFUNCTION`s, and `UPROPERTY`s as public API changes. Consider both native callers and serialized Blueprint assets.
- Preserve Unreal reflection compatibility. Renaming or changing reflected fields and functions requires an explicit migration and deprecation plan.
- A `UUnrealAIClient` must have a suitable `UObject` outer and be retained by a `UPROPERTY` while an HTTP request is active.
- Configure a client before starting a request. Handle `FUnrealAIError` before reading response content and tolerate successful responses with no choices.
- Leave the request model empty when the configured provider default is intended.
- Keep Blueprint async actions and actor components non-blocking. Marshal state and delegate behavior through Unreal-supported runtime primitives.
- Keep module dependencies explicit in `Source/UnrealAI/UnrealAI.Build.cs`. Use public dependencies only when exported headers require them.

## C++ and Blueprint Style

- Follow Unreal naming conventions: `U`, `A`, `F`, `S`, `I`, `E`, and `T` prefixes; PascalCase types and functions; `b`-prefixed booleans.
- Use tabs for C++ indentation and Allman braces, matching the existing source.
- Prefer Unreal containers, strings, smart pointers, delegates, reflection helpers, logging, paths, platform APIs, HTTP, and JSON facilities.
- Keep headers focused and minimize exported dependencies. Include a type's defining header when the complete type is required.
- Expose Blueprint functions and properties deliberately with clear categories, display names, pin behavior, and error paths.
- Blueprint helpers should be safe for empty arrays, absent response content, and invalid JSON.
- Do not put blocking I/O, provider secrets, or platform-specific shell behavior into Blueprint nodes.

## Cross-Platform Requirements

- Ordinary plugin and consumer code must compile without macOS-, Windows-, or Linux-specific branches.
- Use Unreal abstractions such as `FPlatformMisc`, `FPaths`, `FHttpModule`, and platform macros only when a true platform boundary exists.
- Never commit absolute engine installations, home directories, drive-letter user paths, or assumptions about a shell.
- Native Unreal validation must run on a matching host: `Mac` on macOS, `Win64` on Windows, and `Linux` on Linux.
- Do not claim cross-platform compilation from a single-host run. State which hosts were actually exercised.
- Keep GitHub workflow commands valid for their declared shell and runner operating system.

## Security and Privacy

- Never hardcode, log, commit, package, or place API keys in Blueprint assets or screenshots.
- Local credentials belong in process environment variables or an ignored project-root `.env` file. `.env.example` must contain empty or obvious placeholder secret values only.
- Shipped clients must not own hosted-provider secrets; route those requests through a trusted backend.
- Treat prompts, responses, raw JSON, authorization headers, and provider error bodies as potentially sensitive.
- Keep repository and CI tests credential-free. Do not add live provider calls to automated validation.
- Preserve the checks in `Scripts/ci/validate_plugin.py` for credential-like files, token formats, personal paths, and generated output.

## Validation

Use a Python 3 launcher appropriate for the host (`python3`, `python`, or `py -3`). Run portable commands from the repository root:

```text
<python> Scripts/ci/validate_plugin.py
<python> Scripts/ci/validate_skills.py
<python> Scripts/ci/validate_release.py
actionlint
```

Run native packaging and automation on a host with Unreal Engine installed and `UNREAL_ENGINE_ROOT` set using that host's normal environment mechanism:

```text
<python> Scripts/ci/run_unreal_ci.py --platform <Mac|Win64|Linux>
```

Validation expectations:

- Documentation-only changes: portable plugin validation and link/path review.
- Skill changes: both portable validators and the upstream skill schema validator when available.
- Runtime or public API changes: strict native plugin packaging plus all `UnrealAI.*` automation tests.
- Workflow changes: portable validators and `actionlint`.
- Release automation changes: all three portable validators, synchronizer contract tests, and a Release Please dry run when available.
- Provider defaults or Blueprint exposure changes: add or update a native source-of-truth assertion.

Automation test IDs use `UnrealAI.<Area>.<Behavior>`. Add focused regression coverage for behavior changes. Reflection tests should protect documented Blueprint node names, callability, purity, and assignable delegates when those surfaces matter.

## Documentation and Agent Skills

- Update `README.md` for public installation, quickstart, capability, or compatibility changes.
- Update `Documentation/README.md` for detailed API and provider guidance and `Documentation/ContinuousIntegration.md` for validation changes.
- Keep `.env.example`, provider tables, code samples, and source defaults synchronized.
- Update both agent skills when a public C++ or Blueprint contract changes. `Scripts/ci/validate_skills.py` should pin important documented symbols and defaults to source.
- Label generated Blueprint diagrams as illustrations. Do not call them literal editor screenshots.
- Code samples must retain asynchronous UObjects correctly, handle failures first, and avoid real credentials.

## Conventional Commits

All commits use this form:

```text
<type>(optional-scope)!: imperative summary
```

Allowed types:

- `feat`: new user-facing capability
- `fix`: user-facing defect correction
- `docs`: documentation only
- `test`: test-only changes
- `refactor`: behavior-preserving code restructuring
- `perf`: performance improvement
- `build`: build system, packaging, or dependency changes
- `ci`: CI workflow or validation infrastructure
- `chore`: maintenance that fits no other type
- `revert`: revert of an earlier commit

Keep the subject concise, imperative, and without a trailing period. Start lowercase unless the first word is a proper project or API name. Use a scope such as `http`, `blueprint`, `settings`, `ci`, or `docs` only when it adds useful precision. Add a body for motivation, tradeoffs, or migration details. Mark breaking changes with `!` and a `BREAKING CHANGE:` footer.

Keep commits focused and independently understandable. Prefer squash-merging pull requests so the final commit on `main` carries the intended Conventional Commit type and produces a clean changelog entry. Pull requests should summarize behavior and risk, identify public API, config, packaging, cross-platform, or security impacts, and list exact validation commands and results. Include Blueprint or editor images when a visual surface changes.

## Semantic Versioning and Releases

`UnrealAI.uplugin` contains both runtime release fields:

- `VersionName` is the SemVer value without the tag prefix, initially `0.1.0`.
- `Version` is a positive, monotonically increasing Unreal integer, initially `1`.

Release tags use `v<VersionName>`, for example `v0.1.0`. Do not add the `v` prefix inside `UnrealAI.uplugin`.

Release Please owns normal version selection and changelog generation:

1. A Conventional Commit reaches `main`.
2. `.github/workflows/release.yml` creates or updates a release pull request.
3. The release pull request updates `CHANGELOG.md`, `version.txt`, `.release-please-manifest.json`, and `UnrealAI.uplugin`.
4. `Scripts/ci/sync_release_version.py` increments Unreal's integer `Version` once per new semantic release.
5. Merging the release pull request creates the matching `v<VersionName>` tag and published GitHub Release.

Do not manually edit `CHANGELOG.md`, `version.txt`, or `.release-please-manifest.json` during ordinary development. Do not manually bump `UnrealAI.uplugin` for a normal release; review the generated release pull request instead.

Choose the next version from user-visible impact:

- Increment `PATCH` for backward-compatible fixes and performance improvements.
- Increment `MINOR` for backward-compatible features.
- While the project is `0.x`, increment `MINOR` for a breaking public API change and reset `PATCH` to zero.
- Starting with `1.0.0`, increment `MAJOR` for a breaking public API change.
- Documentation, tests, CI, refactors, build maintenance, and chores do not require a release by themselves unless they change the distributed artifact or user-visible behavior.

The highest-impact Conventional Commit since the previous release determines the version increment. Review and merge the generated release pull request only after its version, changelog, descriptor fields, and required validation are correct.
