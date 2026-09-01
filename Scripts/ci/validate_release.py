#!/usr/bin/env python3
"""Validate UnrealAI release automation without creating a tag or release."""

from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

from sync_release_version import synchronize_descriptor_text, target_unreal_version
from validate_plugin import SEMVER_PATTERN


REPOSITORY_ROOT = Path(
    os.environ.get("UNREALAI_REPOSITORY_ROOT", Path(__file__).resolve().parents[2])
).resolve()
CONFIG_PATH = REPOSITORY_ROOT / "release-please-config.json"
MANIFEST_PATH = REPOSITORY_ROOT / ".release-please-manifest.json"
DESCRIPTOR_PATH = REPOSITORY_ROOT / "UnrealAI.uplugin"
VERSION_FILE_PATH = REPOSITORY_ROOT / "version.txt"
CHANGELOG_PATH = REPOSITORY_ROOT / "CHANGELOG.md"
WORKFLOW_PATH = REPOSITORY_ROOT / ".github/workflows/release.yml"
PINNED_ACTION_PATTERN = re.compile(r"uses:\s+([^\s@]+)@([0-9a-f]{40})(?:\s|$)")


def add_error(errors: list[str], message: str) -> None:
    errors.append(message)


def read_json(path: Path, errors: list[str]) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        add_error(errors, f"Unable to read {path.name} as JSON: {exc}")
        return {}
    if not isinstance(value, dict):
        add_error(errors, f"{path.name} must contain a JSON object.")
        return {}
    return value


def validate_configuration(errors: list[str]) -> None:
    config = read_json(CONFIG_PATH, errors)
    manifest = read_json(MANIFEST_PATH, errors)
    descriptor = read_json(DESCRIPTOR_PATH, errors)
    if not config or not descriptor:
        return

    if not re.fullmatch(r"[0-9a-f]{40}", str(config.get("bootstrap-sha", ""))):
        add_error(errors, "release-please bootstrap-sha must be a full commit SHA.")
    if config.get("bump-minor-pre-major") is not True:
        add_error(errors, "Breaking changes before 1.0 must bump the minor version.")
    if config.get("include-component-in-tag") is not False or config.get("include-v-in-tag") is not True:
        add_error(errors, "Release tags must use the v<VersionName> format without a component prefix.")

    packages = config.get("packages")
    package = packages.get(".") if isinstance(packages, dict) else None
    if not isinstance(package, dict):
        add_error(errors, "release-please must configure one package at the repository root.")
        return

    expected_package_values = {
        "release-type": "simple",
        "package-name": "UnrealAI",
        "initial-version": "0.1.0",
        "version-file": "version.txt",
    }
    for key, expected_value in expected_package_values.items():
        if package.get(key) != expected_value:
            add_error(errors, f"release-please package {key} must be {expected_value}.")

    expected_extra_file = {
        "type": "json",
        "path": "UnrealAI.uplugin",
        "jsonpath": "$.VersionName",
    }
    extra_files = package.get("extra-files")
    if not isinstance(extra_files, list) or expected_extra_file not in extra_files:
        add_error(errors, "release-please must update UnrealAI.uplugin VersionName through JSONPath.")

    descriptor_version = descriptor.get("VersionName")
    if not isinstance(descriptor_version, str) or not SEMVER_PATTERN.fullmatch(descriptor_version):
        return

    try:
        version_file = VERSION_FILE_PATH.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError) as exc:
        add_error(errors, f"Unable to read version.txt: {exc}")
    else:
        if version_file != descriptor_version:
            add_error(errors, "version.txt must match UnrealAI.uplugin VersionName.")

    manifest_version = manifest.get(".")
    if manifest_version is not None and manifest_version != descriptor_version:
        add_error(errors, ".release-please-manifest.json must match UnrealAI.uplugin VersionName after a release.")

    try:
        changelog = CHANGELOG_PATH.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        add_error(errors, f"Unable to read CHANGELOG.md: {exc}")
    else:
        if not changelog.startswith("# Changelog\n"):
            add_error(errors, "CHANGELOG.md must begin with a Changelog heading.")


def validate_workflow(errors: list[str]) -> None:
    try:
        workflow = WORKFLOW_PATH.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        add_error(errors, f"Unable to read the release workflow: {exc}")
        return

    if "branches: [main]" not in workflow:
        add_error(errors, "The release workflow must run on pushes to main.")
    if "contents: write" not in workflow or "pull-requests: write" not in workflow:
        add_error(errors, "The release workflow is missing required least-privilege write permissions.")

    pinned_actions = {name for name, _ in PINNED_ACTION_PATTERN.findall(workflow)}
    for required_action in ("actions/checkout", "googleapis/release-please-action"):
        if required_action not in pinned_actions:
            add_error(errors, f"The release workflow must pin {required_action} to a full commit SHA.")


def validate_synchronizer(errors: list[str]) -> None:
    base = {"VersionName": "0.1.0", "Version": 1}
    if target_unreal_version(base, {"VersionName": "0.1.0", "Version": 99}) != 1:
        add_error(errors, "Initial release must retain Unreal integer Version 1.")
    if target_unreal_version(base, {"VersionName": "0.2.0", "Version": 1}) != 2:
        add_error(errors, "A new semantic release must increment the Unreal integer Version once.")

    base_text = '{\n\t"FileVersion": 3,\n\t"Version": 7,\n\t"VersionName": "1.2.3"\n}\n'
    release_text = '{\n\t"FileVersion": 3,\n\t"Version": 7,\n\t"VersionName": "1.3.0"\n}\n'
    updated_text, target_version = synchronize_descriptor_text(base_text, release_text)
    if target_version != 8 or '\t"Version": 8,' not in updated_text:
        add_error(errors, "Release synchronization did not preserve JSON while incrementing Version.")

    try:
        target_unreal_version(base, {"VersionName": "0.0.9", "Version": 1})
    except ValueError:
        pass
    else:
        add_error(errors, "Release synchronization must reject semantic version downgrades.")


def main() -> int:
    errors: list[str] = []
    validate_configuration(errors)
    validate_workflow(errors)
    validate_synchronizer(errors)

    if errors:
        print("UnrealAI release validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print("UnrealAI release automation validation passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
