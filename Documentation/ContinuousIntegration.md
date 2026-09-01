# Continuous integration

UnrealAI uses two CI tiers so that inexpensive checks run on every change while engine-dependent work runs only on an Unreal-equipped machine.

## Workflows

### Validate

`.github/workflows/validate.yml` runs on every push and pull request using a GitHub-hosted Ubuntu runner. It:

- validates `UnrealAI.uplugin`, its declared module layout, a positive integer `Version`, and a SemVer-compliant `VersionName`;
- rejects committed Unreal-generated output, local `.env` files, common credential formats, personal filesystem paths, stale pre-rename identifiers, conflict markers, and trailing whitespace;
- checks that `.env.example` has no populated secret values;
- lints GitHub Actions workflows with `actionlint`;
- lints CI shell scripts with ShellCheck.

The workflow has read-only repository permissions, disables persisted checkout credentials, pins actions to full commit SHAs, and cancels superseded validation runs.

### Unreal Engine

`.github/workflows/unreal-engine.yml` is manually dispatched and runs a native-host matrix on self-hosted macOS, Windows, and Linux runners with the `unreal-engine` label. Each matrix job:

1. runs the same portable validation;
2. packages the plugin for `Mac`, `Win64`, or `Linux` with `BuildPlugin -Rocket -StrictIncludes`;
3. installs the packaged result into a temporary host project;
4. runs the `UnrealAI.*` native automation tests headlessly with `UnrealEditor-Cmd` and `-NullRHI`;
5. parses the exported `index.json` report to make failures and incomplete tests fail the job;
6. uploads the packaged plugin and test report for seven days.

This workflow is manual because standard GitHub-hosted runners do not include the licensed, very large Unreal Engine toolchain, and GitHub warns against automatically running public pull-request code on persistent self-hosted runners.

### Release

`.github/workflows/release.yml` runs after changes reach `main` and uses Release Please in manifest mode:

1. Conventional Commits since the previous release determine the next semantic version.
2. The workflow creates or updates a release pull request containing `CHANGELOG.md`, `version.txt`, `.release-please-manifest.json`, and the new `UnrealAI.uplugin` `VersionName`.
3. A repository script keeps Unreal's positive integer `Version` at `1` for the initial `0.1.0` release and increments it exactly once for each later semantic release.
4. Merging the generated release pull request creates the `v<VersionName>` Git tag and a published GitHub Release using the generated changelog entry.

The first generated release is bootstrapped as `v0.1.0`. Before `1.0.0`, a breaking Conventional Commit increments the minor version; features increment minor, and fixes increment patch. Documentation and maintenance commits can appear in a changelog alongside a releasable change but do not create a release by themselves.

The workflow requests only `contents: write`, `issues: write`, and `pull-requests: write`, uses a concurrency lock, and pins third-party actions to complete commit SHAs. Candidate branches are treated as data: version synchronization and validation execute scripts loaded from trusted `main`, not code from the generated branch. In **Settings → Actions → General**, allow GitHub Actions to create pull requests. The built-in `GITHUB_TOKEN` is sufficient for release management, but GitHub intentionally prevents events created by that token from starting other workflows. If branch protection requires normal CI checks on the generated release pull request, configure a fine-grained `RELEASE_PLEASE_TOKEN` secret with repository contents, issues, and pull-request access; the workflow uses it when present and otherwise falls back to `GITHUB_TOKEN`.

Prefer squash merges with a Conventional Commit-formatted pull request title. Release Please uses the final commits on `main` to calculate both the version and changelog. Do not manually edit the automation-owned manifest, `version.txt`, or changelog during ordinary development.

## Runner setup

1. Register one native self-hosted runner for each desired platform in a restricted organization runner group.
2. Keep each runner's automatic OS label (`macos`, `windows`, or `linux`) and add the custom label `unreal-engine`.
3. Install the same supported Unreal Engine release on every runner, plus Xcode on macOS, Visual Studio with the C++ workload on Windows, and Unreal's required Clang/toolchain packages on Linux. Install `xvfb-run` on headless Linux runners.
4. Create the GitHub environments `unreal-macos`, `unreal-windows`, and `unreal-linux`.
5. In each environment, add an Actions variable named `UNREAL_ENGINE_ROOT` containing that runner's platform-specific Unreal Engine root directory. This path is not a secret.
6. Run the `Unreal Engine` workflow from the Actions tab.

Do not configure API keys on this runner for compilation or unit tests; UnrealAI's current tests are intentionally offline.

For a public repository, keep the engine workflow manual and restrict who can dispatch it. Prefer isolated or ephemeral runners, especially when expanding to pull-request execution. Add engine-version labels or runner groups when the project begins supporting multiple Unreal Engine releases.

## Local commands

Portable validation:

```bash
python3 Scripts/ci/validate_plugin.py
python3 Scripts/ci/validate_skills.py
python3 Scripts/ci/validate_release.py
actionlint
python3 -m compileall -q Scripts/ci
```

Native package and automation test:

```bash
UNREAL_ENGINE_ROOT=/path/to/UnrealEngine python3 Scripts/ci/run_unreal_ci.py --platform Mac
```

Use `--platform Win64` on Windows or `--platform Linux` on Linux. The driver selects the correct `RunUAT` launcher and `UnrealEditor-Cmd` executable for the host operating system.

Set `UNREAL_CI_OUTPUT_DIR` to an empty directory when the package and report should be retained at a known location.

## Why these checks

- Epic documents `BuildPlugin` as the installed-build path used to compile and package standalone code plugins, including the `-Rocket` flow used for distribution: [Unreal Engine Marketplace Guidelines](https://www.unrealengine.com/en-US/marketplace-guidelines).
- Epic's plugin documentation identifies `Binaries` and `Intermediate` as generated plugin output and the `.uplugin` descriptor as JSON consumed by UnrealBuildTool: [Plugins in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/plugins-in-unreal-engine).
- Epic supports command-line automation execution and JSON/HTML report export, which makes native tests suitable for a headless CI gate: [Run Automation Tests](https://dev.epicgames.com/documentation/en-us/unreal-engine/run-automation-tests-in-unreal-engine) and [Review Test Results](https://dev.epicgames.com/documentation/unreal-engine/review-test-results-in-unreal-engine).
- Epic recommends keeping smoke tests below one second and using the automation framework for API-level unit and feature tests: [Automation Test Framework](https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-test-framework-in-unreal-engine).
- GitHub recommends least-privilege token permissions, full-SHA action pinning, and strong isolation for self-hosted runners: [Secure use reference](https://docs.github.com/en/actions/reference/security/secure-use) and [Self-hosted runners](https://docs.github.com/en/actions/concepts/runners/self-hosted-runners).
