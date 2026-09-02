#!/usr/bin/env python3
"""Synchronize Unreal's integer plugin version in a Release Please pull request."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


DEFAULT_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
STABLE_SEMVER_PATTERN = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
UNREAL_VERSION_PATTERN = re.compile(r'(?m)^(\s*"Version"\s*:\s*)\d+(\s*,\s*)$')


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--base-ref",
        default="origin/main",
        help="Git ref containing the previously released UnrealAI.uplugin.",
    )
    parser.add_argument(
        "--repository-root",
        default=str(DEFAULT_REPOSITORY_ROOT),
        help="Repository checkout containing the release candidate descriptor.",
    )
    return parser.parse_args()


def parse_descriptor(content: str, source: str) -> dict[str, object]:
    try:
        descriptor = json.loads(content)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{source} is not valid JSON: {exc}") from exc

    version_name = descriptor.get("VersionName")
    if not isinstance(version_name, str) or not STABLE_SEMVER_PATTERN.fullmatch(version_name):
        raise ValueError(f"{source} VersionName must be a stable semantic version.")

    unreal_version = descriptor.get("Version")
    if isinstance(unreal_version, bool) or not isinstance(unreal_version, int) or unreal_version < 1:
        raise ValueError(f"{source} Version must be a positive integer.")

    return descriptor


def semver_tuple(version_name: str) -> tuple[int, int, int]:
    match = STABLE_SEMVER_PATTERN.fullmatch(version_name)
    if not match:
        raise ValueError(f"Release version must be stable SemVer: {version_name}")
    return tuple(int(part) for part in match.groups())


def target_unreal_version(base_descriptor: dict[str, object], release_descriptor: dict[str, object]) -> int:
    base_version_name = str(base_descriptor["VersionName"])
    release_version_name = str(release_descriptor["VersionName"])
    base_semver = semver_tuple(base_version_name)
    release_semver = semver_tuple(release_version_name)

    if release_semver < base_semver:
        raise ValueError(
            f"Release version {release_version_name} cannot be older than main version {base_version_name}."
        )

    base_unreal_version = int(base_descriptor["Version"])
    return base_unreal_version if release_semver == base_semver else base_unreal_version + 1


def synchronize_descriptor_text(base_content: str, release_content: str) -> tuple[str, int]:
    base_descriptor = parse_descriptor(base_content, "base descriptor")
    release_descriptor = parse_descriptor(release_content, "release descriptor")
    target_version = target_unreal_version(base_descriptor, release_descriptor)

    updated_content, replacement_count = UNREAL_VERSION_PATTERN.subn(
        rf"\g<1>{target_version}\g<2>",
        release_content,
        count=1,
    )
    if replacement_count != 1:
        raise ValueError('Release descriptor must contain one top-level "Version" integer line.')
    return updated_content, target_version


def read_descriptor_at_ref(base_ref: str, repository_root: Path) -> str:
    try:
        result = subprocess.run(
            ["git", "show", f"{base_ref}:UnrealAI.uplugin"],
            cwd=repository_root,
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(f"Unable to read UnrealAI.uplugin from {base_ref}: {exc}") from exc
    return result.stdout


def main() -> int:
    arguments = parse_arguments()
    repository_root = Path(arguments.repository_root).resolve()
    descriptor_path = repository_root / "UnrealAI.uplugin"
    base_content = read_descriptor_at_ref(arguments.base_ref, repository_root)
    release_content = descriptor_path.read_text(encoding="utf-8")
    updated_content, target_version = synchronize_descriptor_text(base_content, release_content)

    if updated_content != release_content:
        descriptor_path.write_text(updated_content, encoding="utf-8")
        print(f"Updated UnrealAI.uplugin Version to {target_version}.")
    else:
        print(f"UnrealAI.uplugin Version is already {target_version}.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"Release version synchronization failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
