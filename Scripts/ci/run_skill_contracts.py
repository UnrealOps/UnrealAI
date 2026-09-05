#!/usr/bin/env python3
"""Compile the skill examples and run their credential-free Unreal contracts."""

from __future__ import annotations

import argparse
import json
import os
import platform as host_platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPOSITORY_ROOT / "Scripts/ci/skill_contracts.json"
PLATFORM_CONFIG = {
    "Mac": {
        "host_system": "Darwin",
        "build": Path("Engine/Build/BatchFiles/Mac/Build.sh"),
        "editor": Path("Engine/Binaries/Mac/UnrealEditor-Cmd"),
    },
    "Win64": {
        "host_system": "Windows",
        "build": Path("Engine/Build/BatchFiles/Build.bat"),
        "editor": Path("Engine/Binaries/Win64/UnrealEditor-Cmd.exe"),
    },
    "Linux": {
        "host_system": "Linux",
        "build": Path("Engine/Build/BatchFiles/Linux/Build.sh"),
        "editor": Path("Engine/Binaries/Linux/UnrealEditor-Cmd"),
    },
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=PLATFORM_CONFIG, required=True)
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="Fresh directory for the exported automation report.",
    )
    return parser.parse_args()


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=REPOSITORY_ROOT, check=True)


def native_command(command: list[str]) -> list[str]:
    if os.name == "nt" and Path(command[0]).suffix.lower() in {".bat", ".cmd"}:
        return ["cmd.exe", "/d", "/s", "/c", subprocess.list2cmdline(command)]
    return command


def stage_sample_project(source_project: Path, output_directory: Path) -> Path:
    """Copy the sample so generation and compilation cannot rewrite the checkout."""
    plugin_collection_directory = output_directory / "ExternalPlugins"
    staged_plugin_directory = plugin_collection_directory / "UnrealAI"
    staged_plugin_directory.mkdir(parents=True)
    shutil.copy2(REPOSITORY_ROOT / "UnrealAI.uplugin", staged_plugin_directory / "UnrealAI.uplugin")
    for directory_name in ("Config", "Resources", "Source"):
        source_directory = REPOSITORY_ROOT / directory_name
        if source_directory.is_dir():
            shutil.copytree(
                source_directory,
                staged_plugin_directory / directory_name,
                ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
            )

    staged_directory = output_directory / "SampleProject"
    shutil.copytree(
        source_project.parent,
        staged_directory,
        ignore=shutil.ignore_patterns(
            "Binaries",
            "DerivedDataCache",
            "Intermediate",
            "Saved",
            ".vs",
            ".idea",
            ".DS_Store",
        ),
    )
    staged_project = staged_directory / source_project.name
    try:
        descriptor = json.loads(staged_project.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Unable to prepare isolated sample project {staged_project}: {exc}") from exc
    descriptor["AdditionalPluginDirectories"] = [str(plugin_collection_directory)]
    staged_project.write_text(json.dumps(descriptor, indent="\t") + "\n", encoding="utf-8")
    return staged_project


def require_file(path: Path, description: str, *, executable: bool = True) -> None:
    if not path.is_file():
        raise RuntimeError(f"{description} was not found: {path}")
    if executable and os.name != "nt" and not os.access(path, os.X_OK):
        raise RuntimeError(f"{description} is not executable: {path}")


def load_contract() -> dict[str, object]:
    try:
        contract = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"Unable to read {CONTRACT_PATH}: {exc}") from exc
    if not isinstance(contract, dict):
        raise RuntimeError(f"Skill contract must be a JSON object: {CONTRACT_PATH}")
    return contract


def contract_test_groups(contract: dict[str, object]) -> list[tuple[str, str, list[str]]]:
    """Return every automation filter/report group required by the contract."""
    groups: list[tuple[str, str, list[str]]] = []
    for report_name, filter_key, tests_key in (
        ("SampleAutomationReport", "automationFilter", "requiredSampleTests"),
        ("PluginAutomationReport", "pluginAutomationFilter", "requiredPluginTests"),
    ):
        automation_filter = contract.get(filter_key)
        required_tests = contract.get(tests_key)
        if not isinstance(automation_filter, str) or not automation_filter.strip():
            raise RuntimeError(f"{filter_key} must be a nonempty test filter.")
        if (
            not isinstance(required_tests, list)
            or not required_tests
            or not all(isinstance(item, str) and item for item in required_tests)
        ):
            raise RuntimeError(f"{tests_key} must be a nonempty array of test names.")
        groups.append((report_name, automation_filter, required_tests))
    return groups


def report_check_command(report_directory: Path, required_tests: list[str]) -> list[str]:
    command = [
        sys.executable,
        str(REPOSITORY_ROOT / "Scripts/ci/check_automation_report.py"),
        str(report_directory / "index.json"),
    ]
    for test_name in required_tests:
        command.extend(("--require", test_name))
    return command


def editor_command(
    editor: Path,
    project: Path,
    report_directory: Path,
    automation_filter: str,
    target_platform: str,
) -> list[str]:
    command = [str(editor)]
    if target_platform == "Linux" and not os.environ.get("DISPLAY"):
        xvfb_run = shutil.which("xvfb-run")
        if not xvfb_run:
            raise RuntimeError("Linux headless testing requires xvfb-run when DISPLAY is unset.")
        command = [xvfb_run, "-a", *command]

    return [
        *command,
        str(project),
        "-unattended",
        "-nop4",
        "-NullRHI",
        "-nosplash",
        "-nosound",
        "-stdout",
        "-FullStdOutLogOutput",
        f"-ExecCmds=Automation RunTests {automation_filter}",
        "-TestExit=Automation Test Queue Empty",
        f"-ReportExportPath={report_directory}",
    ]


def generator_command(editor: Path, project: Path, target_platform: str) -> list[str]:
    command = [str(editor)]
    if target_platform == "Linux" and not os.environ.get("DISPLAY"):
        xvfb_run = shutil.which("xvfb-run")
        if not xvfb_run:
            raise RuntimeError("Linux headless generation requires xvfb-run when DISPLAY is unset.")
        command = [xvfb_run, "-a", *command]

    return [
        *command,
        str(project),
        "-run=UnrealAISampleGenerate",
        "-unattended",
        "-nop4",
        "-NullRHI",
        "-nosplash",
        "-nosound",
        "-stdout",
        "-FullStdOutLogOutput",
    ]


def main() -> int:
    arguments = parse_arguments()
    target_platform = arguments.platform
    config = PLATFORM_CONFIG[target_platform]
    expected_system = str(config["host_system"])
    detected_system = host_platform.system()
    if detected_system != expected_system:
        raise RuntimeError(
            f"Target platform {target_platform} requires a {expected_system} host, "
            f"but this runner reports {detected_system}."
        )

    engine_root_value = os.environ.get("UNREAL_ENGINE_ROOT", "").strip()
    if not engine_root_value:
        raise RuntimeError("UNREAL_ENGINE_ROOT must point to an Unreal Engine installation.")

    contract = load_contract()
    source_project = REPOSITORY_ROOT / str(contract["sampleProject"])
    target = str(contract["editorTarget"])
    test_groups = contract_test_groups(contract)

    engine_root = Path(engine_root_value).expanduser().resolve()
    build = engine_root / Path(config["build"])
    editor = engine_root / Path(config["editor"])
    require_file(build, "Unreal Build Tool launcher")
    require_file(editor, "Unreal Editor command executable")
    require_file(source_project, "UnrealAI sample project", executable=False)

    output_directory = (
        arguments.output_dir.expanduser().resolve()
        if arguments.output_dir
        else Path(tempfile.mkdtemp(prefix="unrealai-skill-contracts-"))
    )
    if output_directory.exists() and any(output_directory.iterdir()):
        raise RuntimeError(f"Skill-contract output directory is not empty: {output_directory}")
    run([sys.executable, str(REPOSITORY_ROOT / "Scripts/ci/validate_skills.py")])
    project = stage_sample_project(source_project, output_directory)
    run(
        native_command(
            [
                str(build),
                target,
                target_platform,
                "Development",
                str(project),
                "-WaitMutex",
                "-NoHotReloadFromIDE",
            ]
        )
    )
    run(generator_command(editor, project, target_platform))
    from response_fixture import response_fixture
    with response_fixture() as fixture_port:
        for report_name, automation_filter, required_tests in test_groups:
            report_directory = output_directory / report_name
            report_directory.mkdir(parents=True, exist_ok=True)
            command = editor_command(
                editor,
                project,
                report_directory,
                automation_filter,
                target_platform,
            )
            command.append(f"-UnrealAIResponseFixturePort={fixture_port}")
            run(command)
            run(report_check_command(report_directory, required_tests))

    print(f"Compiled skill examples and automation reports are available under {output_directory}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (KeyError, OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"UnrealAI skill contracts failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
