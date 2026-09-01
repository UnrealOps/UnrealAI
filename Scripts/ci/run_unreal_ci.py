#!/usr/bin/env python3
"""Package UnrealAI and run its Unreal automation tests on the native host platform."""

from __future__ import annotations

import argparse
import os
import platform as host_platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
PLATFORM_CONFIG = {
    "Mac": {
        "host_system": "Darwin",
        "run_uat": Path("Engine/Build/BatchFiles/RunUAT.sh"),
        "editor": Path("Engine/Binaries/Mac/UnrealEditor-Cmd"),
    },
    "Win64": {
        "host_system": "Windows",
        "run_uat": Path("Engine/Build/BatchFiles/RunUAT.bat"),
        "editor": Path("Engine/Binaries/Win64/UnrealEditor-Cmd.exe"),
    },
    "Linux": {
        "host_system": "Linux",
        "run_uat": Path("Engine/Build/BatchFiles/RunUAT.sh"),
        "editor": Path("Engine/Binaries/Linux/UnrealEditor-Cmd"),
    },
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=PLATFORM_CONFIG, required=True)
    return parser.parse_args()


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=REPOSITORY_ROOT, check=True)


def uat_command(run_uat: Path, arguments: list[str]) -> list[str]:
    command = [str(run_uat), *arguments]
    if os.name == "nt":
        return ["cmd.exe", "/d", "/s", "/c", subprocess.list2cmdline(command)]
    return command


def require_file(path: Path, description: str) -> None:
    if not path.is_file():
        raise RuntimeError(f"{description} was not found: {path}")
    if os.name != "nt" and not os.access(path, os.X_OK):
        raise RuntimeError(f"{description} is not executable: {path}")


def editor_command(editor: Path, project: Path, report_directory: Path, target_platform: str) -> list[str]:
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
        "-ExecCmds=Automation RunTests UnrealAI",
        "-TestExit=Automation Test Queue Empty",
        f"-ReportExportPath={report_directory}",
    ]


def main() -> int:
    arguments = parse_arguments()
    target_platform = arguments.platform
    config = PLATFORM_CONFIG[target_platform]

    detected_system = host_platform.system()
    expected_system = str(config["host_system"])
    if detected_system != expected_system:
        raise RuntimeError(
            f"Target platform {target_platform} requires a {expected_system} host, "
            f"but this runner reports {detected_system}."
        )

    engine_root_value = os.environ.get("UNREAL_ENGINE_ROOT", "").strip()
    if not engine_root_value:
        raise RuntimeError("UNREAL_ENGINE_ROOT must point to an Unreal Engine installation.")

    engine_root = Path(engine_root_value).expanduser().resolve()
    run_uat = engine_root / Path(config["run_uat"])
    editor = engine_root / Path(config["editor"])
    require_file(run_uat, "Unreal Automation Tool launcher")
    require_file(editor, "Unreal Editor command executable")

    output_root_value = os.environ.get("UNREAL_CI_OUTPUT_DIR", "").strip()
    output_root = (
        Path(output_root_value).expanduser().resolve()
        if output_root_value
        else Path(tempfile.mkdtemp(prefix="unrealai-ci-"))
    )
    output_root.mkdir(parents=True, exist_ok=True)

    package_directory = output_root / "Package"
    host_project_directory = output_root / "HostProject"
    report_directory = output_root / "AutomationReport"
    for generated_directory in (package_directory, host_project_directory, report_directory):
        if generated_directory.exists():
            raise RuntimeError(
                f"CI output path already exists: {generated_directory}. "
                "Use a fresh UNREAL_CI_OUTPUT_DIR."
            )

    run([sys.executable, str(REPOSITORY_ROOT / "Scripts/ci/validate_plugin.py")])
    run(
        uat_command(
            run_uat,
            [
                "BuildPlugin",
                f"-Plugin={REPOSITORY_ROOT / 'UnrealAI.uplugin'}",
                f"-Package={package_directory}",
                f"-TargetPlatforms={target_platform}",
                "-Rocket",
                "-StrictIncludes",
            ],
        )
    )

    packaged_plugin_directory = host_project_directory / "Plugins/UnrealAI"
    packaged_plugin_directory.parent.mkdir(parents=True)
    shutil.copytree(package_directory, packaged_plugin_directory)
    shutil.copy2(
        REPOSITORY_ROOT / "Tests/HostProject/UnrealAIHost.uproject",
        host_project_directory / "UnrealAIHost.uproject",
    )
    report_directory.mkdir(parents=True)

    run(
        editor_command(
            editor,
            host_project_directory / "UnrealAIHost.uproject",
            report_directory,
            target_platform,
        )
    )
    run(
        [
            sys.executable,
            str(REPOSITORY_ROOT / "Scripts/ci/check_automation_report.py"),
            str(report_directory / "index.json"),
        ]
    )

    print(f"UnrealAI package and automation report are available under {output_root}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"UnrealAI CI failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
