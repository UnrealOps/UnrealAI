#!/usr/bin/env python3
"""Install optional SDK plugins beside UnrealAI, where Unreal can discover them."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True, type=Path, help="Project directory or .uproject file")
    parser.add_argument("--auth", action="store_true", help="Install UnrealAIAuth")
    parser.add_argument("--experimental", action="store_true", help="Install UnrealAIExperimentalAccess and auth")
    parser.add_argument("--link", action="store_true", help="Link source files for local development, keeping plugin binaries in the sibling directory")
    args = parser.parse_args()
    if not args.auth and not args.experimental:
        parser.error("Select --auth or --experimental.")
    project = args.project.resolve(strict=True)
    if project.suffix == ".uproject":
        project = project.parent
    if not project.is_dir() or not any(project.glob("*.uproject")):
        parser.error("The selected directory must contain an Unreal project.")
    sdk = Path(__file__).resolve().parent.parent
    names = ["UnrealAIAuth"]
    if args.experimental:
        names.append("UnrealAIExperimentalAccess")
    pairs = [(sdk / "Addons" / name, project / "Plugins" / name) for name in names]
    for source, destination in pairs:
        if not (source / (source.name + ".uplugin")).is_file():
            parser.error(f"This SDK checkout does not contain {source.name}.")
        if destination.exists() or destination.is_symlink():
            if args.link and destination.is_symlink() and destination.resolve() == source.resolve():
                continue
            if args.link and (destination / "Source").is_symlink() and (destination / "Source").resolve() == (source / "Source").resolve():
                continue
            parser.error(f"{destination.name} already exists; preserve or remove that installation before replacing it.")
    for source, destination in pairs:
        if args.link and (destination / "Source").is_symlink() and (destination / "Source").resolve() == (source / "Source").resolve():
            shutil.copy2(source / (source.name + ".uplugin"), destination / (source.name + ".uplugin"))
            print(f"Already linked: {destination.name}")
            continue
        if destination.is_symlink() and destination.resolve() == source.resolve():
            destination.unlink()
        destination.parent.mkdir(parents=True, exist_ok=True)
        if args.link:
            # The plugin root must be a real sibling directory: dyld resolves loader-relative
            # dependency paths from the physical binary location, not a directory symlink.
            destination.mkdir()
            for child in source.iterdir():
                if child.name in {"Binaries", "Intermediate", "Saved", "DerivedDataCache", ".git"} or child.name.startswith(".env"):
                    continue
                if child.suffix == ".uplugin":
                    shutil.copy2(child, destination / child.name)
                else:
                    (destination / child.name).symlink_to(child, target_is_directory=child.is_dir())
        else:
            shutil.copytree(source, destination, ignore=shutil.ignore_patterns(
                "Binaries", "Intermediate", "Saved", "DerivedDataCache", ".git", ".env*", "__pycache__"
            ))
        print(f"Installed: {destination.name}")
    print("Enable the desired optional plugin in your project. Experimental access remains disabled by default.")


if __name__ == "__main__":
    main()
