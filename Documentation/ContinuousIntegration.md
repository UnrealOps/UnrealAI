# Continuous integration

UnrealAI uses two CI tiers so that inexpensive checks run on every change while engine-dependent work runs only on an Unreal-equipped machine.

## Workflows

### Validate

`.github/workflows/validate.yml` runs on every push and pull request using a GitHub-hosted Ubuntu runner. It:

- validates `UnrealAI.uplugin` and its declared module layout;
- rejects committed Unreal-generated output, local `.env` files, common credential formats, personal filesystem paths, stale pre-rename identifiers, conflict markers, and trailing whitespace;
- checks that `.env.example` has no populated secret values;
- lints GitHub Actions workflows with `actionlint`;
- lints CI shell scripts with ShellCheck.

The workflow has read-only repository permissions, disables persisted checkout credentials, pins actions to full commit SHAs, and cancels superseded validation runs.

### Unreal Engine

`.github/workflows/unreal-engine.yml` is manually dispatched and targets a self-hosted macOS runner with the `unreal-engine` label. It:

1. runs the same portable validation;
2. packages the plugin with `RunUAT.sh BuildPlugin -Rocket -StrictIncludes`;
3. installs the packaged result into a temporary host project;
4. runs the `UnrealAI.*` native automation tests headlessly with `UnrealEditor-Cmd` and `-NullRHI`;
5. parses the exported `index.json` report to make failures and incomplete tests fail the job;
6. uploads the packaged plugin and test report for seven days.

This workflow is manual because standard GitHub-hosted runners do not include the licensed, very large Unreal Engine toolchain, and GitHub warns against automatically running public pull-request code on persistent self-hosted runners.

## Runner setup

1. Register a macOS self-hosted runner for the repository or a restricted organization runner group.
2. Add the custom runner label `unreal-engine`.
3. Install the supported Unreal Engine version and the matching Xcode toolchain.
4. Add a repository Actions variable named `UNREAL_ENGINE_ROOT` whose value is the Unreal Engine root directory. This is a path, not a secret.
5. Run the `Unreal Engine` workflow from the Actions tab.

Do not configure API keys on this runner for compilation or unit tests; UnrealAI's current tests are intentionally offline.

For a public repository, keep the engine workflow manual and restrict who can dispatch it. For broader platform or engine-version coverage, use isolated or ephemeral runners with distinct labels and run `BuildPlugin` once per supported engine/platform combination.

## Local commands

Portable validation:

```bash
python3 Scripts/ci/validate_plugin.py
actionlint
shellcheck Scripts/ci/*.sh
```

macOS package and native automation test:

```bash
UNREAL_ENGINE_ROOT=/path/to/UnrealEngine Scripts/ci/run-unreal-ci.sh
```

Set `UNREAL_CI_OUTPUT_DIR` to an empty directory when the package and report should be retained at a known location.

## Why these checks

- Epic documents `BuildPlugin` as the installed-build path used to compile and package standalone code plugins, including the `-Rocket` flow used for distribution: [Unreal Engine Marketplace Guidelines](https://www.unrealengine.com/en-US/marketplace-guidelines).
- Epic's plugin documentation identifies `Binaries` and `Intermediate` as generated plugin output and the `.uplugin` descriptor as JSON consumed by UnrealBuildTool: [Plugins in Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/plugins-in-unreal-engine).
- Epic supports command-line automation execution and JSON/HTML report export, which makes native tests suitable for a headless CI gate: [Run Automation Tests](https://dev.epicgames.com/documentation/en-us/unreal-engine/run-automation-tests-in-unreal-engine) and [Review Test Results](https://dev.epicgames.com/documentation/unreal-engine/review-test-results-in-unreal-engine).
- Epic recommends keeping smoke tests below one second and using the automation framework for API-level unit and feature tests: [Automation Test Framework](https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-test-framework-in-unreal-engine).
- GitHub recommends least-privilege token permissions, full-SHA action pinning, and strong isolation for self-hosted runners: [Secure use reference](https://docs.github.com/en/actions/reference/security/secure-use) and [Self-hosted runners](https://docs.github.com/en/actions/concepts/runners/self-hosted-runners).
