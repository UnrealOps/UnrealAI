#!/usr/bin/env python3
"""Validate UnrealAI without requiring an Unreal Engine installation."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(
    os.environ.get("UNREALAI_REPOSITORY_ROOT", Path(__file__).resolve().parents[2])
).resolve()
DESCRIPTOR_PATH = REPOSITORY_ROOT / "UnrealAI.uplugin"
SAMPLE_ROOT = REPOSITORY_ROOT / "Samples" / "UnrealAISample"
SAMPLE_DESCRIPTOR_PATH = SAMPLE_ROOT / "UnrealAISample.uproject"
HOST_DESCRIPTOR_PATH = REPOSITORY_ROOT / "Tests" / "HostProject" / "UnrealAIHost.uproject"
GENERATED_DIRECTORIES = {"Binaries", "DerivedDataCache", "Intermediate", "Saved"}
CREDENTIAL_FILENAMES = {
    ".env",
    "credentials.json",
    "id_dsa",
    "id_ecdsa",
    "id_ed25519",
    "id_rsa",
}
CREDENTIAL_SUFFIXES = {".key", ".keystore", ".p12", ".pem", ".pfx"}
SECRET_PATTERNS = {
    "OpenAI-style API key": re.compile(r"\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{20,}\b"),
    "xAI API key": re.compile(r"\bxai-[A-Za-z0-9_-]{20,}\b"),
    "AWS access key": re.compile(r"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b"),
    "GitHub token": re.compile(r"\b(?:github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,})\b"),
    "Google API key": re.compile(r"\bAIza[A-Za-z0-9_-]{30,}\b"),
    "Slack token": re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}\b"),
    "JWT": re.compile(r"\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b"),
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----"),
    "credential in URL": re.compile(r"https?://[^\s/:]+:[^\s/@]+@"),
    "non-empty sensitive configuration value": re.compile(
        r"(?im)^\s*(?:api_?key|password|secret|security_?token)\s*=\s*"
        r"(?!(?:your[_-]|example|placeholder|<)[^\r\n]*$)\S+[^\r\n]*$"
    ),
}
PERSONAL_PATH_PATTERN = re.compile(
    r"(?:/Users/[A-Za-z0-9._-]+(?:/|\b)|/home/[A-Za-z0-9._-]+(?:/|\b)|[A-Za-z]:\\Users\\[A-Za-z0-9._-]+(?:\\|\b))"
)
CONFLICT_MARKER_PATTERN = re.compile(r"^(?:<{7}|={7}|>{7})(?:\s|$)")
STALE_IDENTIFIERS = ("OpenAICompat" + "AI",)
SEMVER_PATTERN = re.compile(
    r"^(?:0|[1-9]\d*)\."
    r"(?:0|[1-9]\d*)\."
    r"(?:0|[1-9]\d*)"
    r"(?:-(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


def add_error(errors: list[str], message: str) -> None:
    errors.append(message)


def repository_files(errors: list[str]) -> list[Path]:
    try:
        result = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
            cwd=REPOSITORY_ROOT,
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        add_error(errors, f"Unable to enumerate Git files: {exc}")
        return []

    files = [REPOSITORY_ROOT / path.decode("utf-8") for path in result.stdout.split(b"\0") if path]
    return [path for path in files if path.is_file()]


def validate_descriptor(errors: list[str]) -> None:
    try:
        descriptor = json.loads(DESCRIPTOR_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        add_error(errors, f"UnrealAI.uplugin is not valid UTF-8 JSON: {exc}")
        return

    if descriptor.get("FileVersion") != 3:
        add_error(errors, "UnrealAI.uplugin must use FileVersion 3.")

    version = descriptor.get("Version")
    if isinstance(version, bool) or not isinstance(version, int) or version < 1:
        add_error(errors, "UnrealAI.uplugin Version must be a positive integer.")

    version_name = descriptor.get("VersionName")
    if not isinstance(version_name, str) or not SEMVER_PATTERN.fullmatch(version_name):
        add_error(errors, "UnrealAI.uplugin VersionName must be a semantic version without a v prefix.")

    modules = descriptor.get("Modules")
    if not isinstance(modules, list) or not modules:
        add_error(errors, "UnrealAI.uplugin must declare at least one module.")
        return

    seen_names: set[str] = set()
    for module in modules:
        if not isinstance(module, dict):
            add_error(errors, "Every plugin module entry must be a JSON object.")
            continue

        name = module.get("Name")
        if not isinstance(name, str) or not name:
            add_error(errors, "Every plugin module must have a non-empty Name.")
            continue
        if name in seen_names:
            add_error(errors, f"Duplicate plugin module: {name}")
        seen_names.add(name)

        module_directory = REPOSITORY_ROOT / "Source" / name
        if not module_directory.is_dir():
            add_error(errors, f"Module source directory is missing: Source/{name}")
        if not (module_directory / f"{name}.Build.cs").is_file():
            add_error(errors, f"Module rules file is missing: Source/{name}/{name}.Build.cs")
        if not module.get("Type"):
            add_error(errors, f"Module {name} must declare Type.")
        if not module.get("LoadingPhase"):
            add_error(errors, f"Module {name} must declare LoadingPhase.")

    if "UnrealAI" not in seen_names:
        add_error(errors, "The descriptor must declare the UnrealAI module.")


def validate_gitignore(errors: list[str]) -> None:
    gitignore_path = REPOSITORY_ROOT / ".gitignore"
    try:
        rules = {
            line.strip()
            for line in gitignore_path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
    except (OSError, UnicodeError) as exc:
        add_error(errors, f"Unable to read .gitignore: {exc}")
        return

    required_rules = {
        ".env",
        ".env.*",
        "!.env.example",
        "Binaries/",
        "DerivedDataCache/",
        "Intermediate/",
        "Saved/",
        "Samples/*/Build/",
        "Samples/*/Config/DefaultInput.ini",
    }
    for missing_rule in sorted(required_rules - rules):
        add_error(errors, f".gitignore is missing required rule: {missing_rule}")


def validate_env_example(errors: list[str]) -> None:
    env_example_path = REPOSITORY_ROOT / ".env.example"
    try:
        lines = env_example_path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as exc:
        add_error(errors, f"Unable to read .env.example: {exc}")
        return

    names: set[str] = set()
    for line_number, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or "=" not in stripped:
            continue
        name, value = stripped.split("=", 1)
        names.add(name)
        if re.search(r"(?:KEY|PASSWORD|SECRET|TOKEN)$", name, re.IGNORECASE) and value.strip():
            add_error(errors, f".env.example:{line_number} contains a non-empty sensitive value.")

    required_api_key_variables = {
        "OPENAI_API_KEY",
        "XAI_API_KEY",
        "ANTHROPIC_API_KEY",
        "GEMINI_API_KEY",
    }
    for missing_name in sorted(required_api_key_variables - names):
        add_error(errors, f".env.example is missing built-in provider variable: {missing_name}")


def validate_sample_project(errors: list[str]) -> None:
    try:
        descriptor = json.loads(SAMPLE_DESCRIPTOR_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        add_error(errors, f"Sample project descriptor is not valid UTF-8 JSON: {exc}")
        return

    if descriptor.get("AdditionalPluginDirectories") != ["../../.."]:
        add_error(errors, "Sample project must discover the repository plugin through ../../...")

    plugins = descriptor.get("Plugins")
    if not isinstance(plugins, list) or not any(
        isinstance(plugin, dict)
        and plugin.get("Name") == "UnrealAI"
        and plugin.get("Enabled") is True
        for plugin in plugins
    ):
        add_error(errors, "Sample project must enable the UnrealAI plugin.")
    if not isinstance(plugins, list) or not any(
        isinstance(plugin, dict)
        and plugin.get("Name") == "AndroidFileServer"
        and plugin.get("Enabled") is False
        for plugin in plugins
    ):
        add_error(errors, "Sample project must disable AndroidFileServer to avoid generated security tokens.")

    modules = descriptor.get("Modules")
    expected_modules = {"UnrealAISample", "UnrealAISampleEditor"}
    declared_modules = {
        module.get("Name")
        for module in modules
        if isinstance(module, dict) and isinstance(module.get("Name"), str)
    } if isinstance(modules, list) else set()
    for missing_module in sorted(expected_modules - declared_modules):
        add_error(errors, f"Sample project is missing module: {missing_module}")
    for module_name in sorted(expected_modules):
        rules_path = SAMPLE_ROOT / "Source" / module_name / f"{module_name}.Build.cs"
        if not rules_path.is_file():
            add_error(errors, f"Sample module rules file is missing: {rules_path.relative_to(REPOSITORY_ROOT)}")

    required_sample_files = (
        SAMPLE_ROOT / "README.md",
        SAMPLE_ROOT / "Content" / "Blueprints" / "BP_UnrealAIGettingStarted.uasset",
        SAMPLE_ROOT / "Content" / "Maps" / "UnrealAISampleMap.umap",
        SAMPLE_ROOT / "Source" / "UnrealAISample" / "Public" / "UnrealAISampleActor.h",
        SAMPLE_ROOT / "Source" / "UnrealAISample" / "Private" / "UnrealAISampleActor.cpp",
        SAMPLE_ROOT / "Source" / "UnrealAISample" / "Public" / "UnrealAIMultiTurnExample.h",
        SAMPLE_ROOT / "Source" / "UnrealAISample" / "Private" / "UnrealAIMultiTurnExample.cpp",
        SAMPLE_ROOT / "Source" / "UnrealAISample" / "Public" / "UnrealAIProductionDeploymentExample.h",
        SAMPLE_ROOT / "Source" / "UnrealAISample" / "Private" / "UnrealAIProductionDeploymentExample.cpp",
        SAMPLE_ROOT / "Source" / "UnrealAISample" / "Public" / "UnrealAIStreamingExample.h",
        SAMPLE_ROOT / "Source" / "UnrealAISample" / "Private" / "UnrealAIStreamingExample.cpp",
    )
    for required_path in required_sample_files:
        if not required_path.is_file():
            add_error(errors, f"Required sample file is missing: {required_path.relative_to(REPOSITORY_ROOT)}")


def validate_host_project(errors: list[str]) -> None:
    try:
        descriptor = json.loads(HOST_DESCRIPTOR_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        add_error(errors, f"Automation host project descriptor is not valid UTF-8 JSON: {exc}")
        return

    plugins = descriptor.get("Plugins")
    if not isinstance(plugins, list) or not any(
        isinstance(plugin, dict)
        and plugin.get("Name") == "AndroidFileServer"
        and plugin.get("Enabled") is False
        for plugin in plugins
    ):
        add_error(
            errors,
            "Automation host project must disable AndroidFileServer so native tests do not depend on its platform module.",
        )


def validate_files(files: list[Path], errors: list[str]) -> None:
    for path in files:
        relative_path = path.relative_to(REPOSITORY_ROOT)
        relative_text = relative_path.as_posix()

        if any(part in GENERATED_DIRECTORIES for part in relative_path.parts):
            add_error(errors, f"Generated output is eligible for commit: {relative_text}")

        if path.name in CREDENTIAL_FILENAMES or path.suffix.lower() in CREDENTIAL_SUFFIXES:
            add_error(errors, f"Credential-like file is eligible for commit: {relative_text}")

        if path.name.startswith(".env.") and path.name != ".env.example":
            add_error(errors, f"Local environment file is eligible for commit: {relative_text}")

        try:
            raw_content = path.read_bytes()
        except OSError as exc:
            add_error(errors, f"Unable to read {relative_text}: {exc}")
            continue

        if b"\0" in raw_content:
            continue

        try:
            content = raw_content.decode("utf-8")
        except UnicodeDecodeError:
            add_error(errors, f"Text file is not valid UTF-8: {relative_text}")
            continue

        for pattern_name, pattern in SECRET_PATTERNS.items():
            if pattern.search(content):
                add_error(errors, f"Possible {pattern_name} found in {relative_text}.")

        if PERSONAL_PATH_PATTERN.search(content):
            add_error(errors, f"Personal filesystem path found in {relative_text}.")

        for stale_identifier in STALE_IDENTIFIERS:
            if stale_identifier in content:
                add_error(errors, f"Stale pre-rename identifier found in {relative_text}.")

        for line_number, line in enumerate(content.splitlines(), start=1):
            if CONFLICT_MARKER_PATTERN.match(line):
                add_error(errors, f"Merge-conflict marker found at {relative_text}:{line_number}.")
            if line.endswith((" ", "\t")):
                add_error(errors, f"Trailing whitespace found at {relative_text}:{line_number}.")


def main() -> int:
    errors: list[str] = []
    files = repository_files(errors)
    validate_descriptor(errors)
    validate_gitignore(errors)
    validate_env_example(errors)
    validate_sample_project(errors)
    validate_host_project(errors)
    validate_files(files, errors)
    from validate_sdk_boundaries import validate as validate_sdk_boundaries
    validate_sdk_boundaries(REPOSITORY_ROOT, errors)

    if errors:
        print("UnrealAI validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(f"UnrealAI validation passed for {len(files)} committable files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
