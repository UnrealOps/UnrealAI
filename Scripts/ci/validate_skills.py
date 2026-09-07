#!/usr/bin/env python3
"""Lint repository-local UnrealAI skill packaging and native-contract wiring."""

from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(
    os.environ.get("UNREALAI_REPOSITORY_ROOT", Path(__file__).resolve().parents[2])
).resolve()
SKILLS_ROOT = REPOSITORY_ROOT / ".agents" / "skills"
CONTRACT_PATH = REPOSITORY_ROOT / "Scripts/ci/skill_contracts.json"
STREAMING_EXAMPLE_HEADER_PATH = (
    REPOSITORY_ROOT
    / "Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIStreamingExample.h"
)
STREAMING_EXAMPLE_SOURCE_PATH = (
    REPOSITORY_ROOT
    / "Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIStreamingExample.cpp"
)
STREAMING_REFERENCE_PATH = SKILLS_ROOT / "unrealai-cpp/references/streaming-client.md"

SKILL_PACKAGES = {
    "unrealai-cpp": (
        "references/native-sdk.md",
        "references/responses-client.md",
        "references/client-setup.md",
        "references/one-shot-client.md",
        "references/streaming-client.md",
        "references/chat-component.md",
        "references/multi-turn-client.md",
        "references/streaming-conversation.md",
        "references/mixed-integration.md",
        "references/packaged-client-deployment.md",
        "references/dedicated-server-deployment.md",
        "references/backend-deployment.md",
    ),
    "unrealai-blueprints": (
        "references/responses-actions.md",
        "references/async-actions.md",
        "references/chat-component.md",
        "references/requests-and-providers.md",
        "references/asset-workflow.md",
        "references/multi-turn-component.md",
        "references/mixed-integration.md",
        "references/backend-streaming-conversation.md",
        "references/packaged-client-deployment.md",
        "references/dedicated-server-deployment.md",
        "references/backend-deployment.md",
    ),
}

MARKDOWN_LINK_PATTERN = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
QUOTED_YAML_VALUE_PATTERN = re.compile(r'^\s{2}([a-z_]+):\s+"([^"]*)"\s*$', re.MULTILINE)
FORBIDDEN_PORTABILITY_TEXT = (
    "/Applications/Epic Games/",
    "/Users/",
    "C:\\Users\\",
    "RunUAT.sh",
    "RunUAT.bat",
    "export OPENAI_API_KEY",
    "$env:OPENAI_API_KEY",
)


def add_error(errors: list[str], message: str) -> None:
    errors.append(message)


def read_text(path: Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        add_error(errors, f"Unable to read {path.relative_to(REPOSITORY_ROOT)}: {exc}")
        return ""


def extract_marked_cpp_block(content: str, label: str, errors: list[str]) -> str:
    begin = f"<!-- BEGIN {label} -->\n```cpp\n"
    end = f"\n```\n<!-- END {label} -->"
    if content.count(begin) != 1 or content.count(end) != 1:
        add_error(errors, f"The C++ skill reference must contain one marked {label.lower()} block.")
        return ""

    block_start = content.index(begin) + len(begin)
    block_end = content.index(end, block_start)
    return content[block_start:block_end]


def parse_frontmatter(skill_path: Path, content: str, errors: list[str]) -> dict[str, str]:
    relative_path = skill_path.relative_to(REPOSITORY_ROOT)
    if not content.startswith("---\n"):
        add_error(errors, f"{relative_path} must begin with YAML frontmatter.")
        return {}

    try:
        frontmatter, _ = content[4:].split("\n---\n", 1)
    except ValueError:
        add_error(errors, f"{relative_path} has unterminated YAML frontmatter.")
        return {}

    values: dict[str, str] = {}
    for line in frontmatter.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        values[key.strip()] = value.strip().strip('"')
    return values


def validate_links(skill_directory: Path, files: list[Path], errors: list[str]) -> None:
    for path in files:
        content = read_text(path, errors)
        for raw_target in MARKDOWN_LINK_PATTERN.findall(content):
            target = raw_target.split("#", 1)[0]
            if not target or "://" in target:
                continue
            resolved = (path.parent / target).resolve()
            try:
                resolved.relative_to(skill_directory.resolve())
            except ValueError:
                add_error(errors, f"{path.relative_to(REPOSITORY_ROOT)} links outside its skill: {raw_target}")
                continue
            if not resolved.is_file():
                add_error(errors, f"{path.relative_to(REPOSITORY_ROOT)} has a missing link: {raw_target}")


def validate_openai_yaml(skill_name: str, path: Path, errors: list[str]) -> None:
    content = read_text(path, errors)
    fields = dict(QUOTED_YAML_VALUE_PATTERN.findall(content))
    if not fields.get("display_name"):
        add_error(errors, f"{path.relative_to(REPOSITORY_ROOT)} is missing interface.display_name.")

    short_description = fields.get("short_description", "")
    if not 25 <= len(short_description) <= 64:
        add_error(errors, f"{path.relative_to(REPOSITORY_ROOT)} short_description must be 25-64 characters.")

    if f"${skill_name}" not in fields.get("default_prompt", ""):
        add_error(errors, f"{path.relative_to(REPOSITORY_ROOT)} default_prompt must mention ${skill_name}.")


def validate_skill_package(skill_name: str, references: tuple[str, ...], errors: list[str]) -> None:
    skill_directory = SKILLS_ROOT / skill_name
    skill_path = skill_directory / "SKILL.md"
    reference_paths = [skill_directory / reference for reference in references]
    openai_yaml_path = skill_directory / "agents/openai.yaml"
    expected_files = (skill_path, *reference_paths, openai_yaml_path)

    for path in expected_files:
        if not path.is_file():
            add_error(errors, f"Required skill file is missing: {path.relative_to(REPOSITORY_ROOT)}")

    expected_reference_paths = set(references)
    actual_reference_paths = {
        path.relative_to(skill_directory).as_posix()
        for path in (skill_directory / "references").glob("*.md")
        if path.is_file()
    }
    for unexpected_reference in sorted(actual_reference_paths - expected_reference_paths):
        add_error(
            errors,
            f"{skill_name} has an orphaned reference not routed by SKILL.md: {unexpected_reference}",
        )
    if not all(path.is_file() for path in expected_files):
        return

    skill_content = read_text(skill_path, errors)
    reference_content = "\n".join(read_text(path, errors) for path in reference_paths)
    combined_content = f"{skill_content}\n{reference_content}"
    frontmatter = parse_frontmatter(skill_path, skill_content, errors)

    if frontmatter.get("name") != skill_name:
        add_error(errors, f"{skill_path.relative_to(REPOSITORY_ROOT)} frontmatter name must be {skill_name}.")
    description = frontmatter.get("description", "")
    if len(description) < 40 or "TODO" in description:
        add_error(errors, f"{skill_path.relative_to(REPOSITORY_ROOT)} needs a specific description.")
    if "TODO" in combined_content or "[TODO" in combined_content:
        add_error(errors, f"{skill_name} contains an unfinished scaffold placeholder.")

    for reference in references:
        if f"]({reference})" not in skill_content:
            add_error(errors, f"{skill_name} does not route to {reference}.")

    validate_links(skill_directory, [skill_path, *reference_paths], errors)
    validate_openai_yaml(skill_name, openai_yaml_path, errors)

    for forbidden_text in FORBIDDEN_PORTABILITY_TEXT:
        if forbidden_text in combined_content:
            add_error(errors, f"{skill_name} contains platform-specific instruction: {forbidden_text}")


def require_contract_string(
    contract: dict[str, object],
    field: str,
    errors: list[str],
) -> str:
    value = contract.get(field)
    if not isinstance(value, str) or not value.strip():
        add_error(errors, f"{CONTRACT_PATH.relative_to(REPOSITORY_ROOT)} field {field} must be a non-empty string.")
        return ""
    return value


def require_contract_tests(
    contract: dict[str, object],
    field: str,
    prefix: str,
    errors: list[str],
) -> list[str]:
    value = contract.get(field)
    if not isinstance(value, list) or not value or not all(isinstance(item, str) for item in value):
        add_error(errors, f"{CONTRACT_PATH.relative_to(REPOSITORY_ROOT)} field {field} must be a non-empty string array.")
        return []

    tests = list(value)
    if len(set(tests)) != len(tests):
        add_error(errors, f"{CONTRACT_PATH.relative_to(REPOSITORY_ROOT)} field {field} contains duplicate tests.")
    for test_name in tests:
        if not test_name.startswith(prefix):
            add_error(errors, f"Contract test {test_name} must use the {prefix} prefix.")
    return tests


def validate_native_contract_wiring(errors: list[str]) -> None:
    try:
        contract = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        add_error(errors, f"Unable to read {CONTRACT_PATH.relative_to(REPOSITORY_ROOT)}: {exc}")
        return
    if not isinstance(contract, dict):
        add_error(errors, f"{CONTRACT_PATH.relative_to(REPOSITORY_ROOT)} must contain a JSON object.")
        return
    if contract.get("schemaVersion") != 1:
        add_error(errors, f"{CONTRACT_PATH.relative_to(REPOSITORY_ROOT)} schemaVersion must be 1.")

    project_value = require_contract_string(contract, "sampleProject", errors)
    target = require_contract_string(contract, "editorTarget", errors)
    automation_filter = require_contract_string(contract, "automationFilter", errors)
    sample_tests = require_contract_tests(
        contract,
        "requiredSampleTests",
        "UnrealAISample.SkillContracts.",
        errors,
    )
    require_contract_tests(contract, "requiredPluginTests", "UnrealAI.", errors)

    if project_value:
        project_path = (REPOSITORY_ROOT / project_value).resolve()
        try:
            project_path.relative_to(REPOSITORY_ROOT)
        except ValueError:
            add_error(errors, "The skill-contract sample project must remain inside the repository.")
        else:
            if not project_path.is_file() or project_path.suffix != ".uproject":
                add_error(errors, f"Skill-contract sample project is missing: {project_value}")

    target_path = REPOSITORY_ROOT / "Samples/UnrealAISample/Source" / f"{target}.Target.cs"
    if target and not target_path.is_file():
        add_error(errors, f"Skill-contract editor target is missing: {target_path.relative_to(REPOSITORY_ROOT)}")
    if automation_filter and any(not test.startswith(f"{automation_filter}.") for test in sample_tests):
        add_error(errors, "Every required sample test must be selected by automationFilter.")

    for relative_path in (
        "Scripts/ci/run_skill_contracts.py",
        "Scripts/ci/check_automation_report.py",
        "Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIMultiTurnExample.h",
        "Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIMultiTurnExample.cpp",
        "Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIPackagedClientFacadeExample.h",
        "Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIPackagedClientFacadeExample.cpp",
        "Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIPackagedChatWidgetExample.h",
        "Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIPackagedChatWidgetExample.cpp",
        "Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIProductionDeploymentExample.h",
        "Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIProductionDeploymentExample.cpp",
        "Samples/UnrealAISample/Source/UnrealAISample/Public/UnrealAIStreamingExample.h",
        "Samples/UnrealAISample/Source/UnrealAISample/Private/UnrealAIStreamingExample.cpp",
        "Samples/UnrealAISample/Source/UnrealAISampleEditor/Private/UnrealAISampleGenerateCommandlet.cpp",
        "Samples/UnrealAISample/Source/UnrealAISampleEditor/Private/Tests/UnrealAISampleAutomationTests.cpp",
    ):
        if not (REPOSITORY_ROOT / relative_path).is_file():
            add_error(errors, f"Native skill-contract input is missing: {relative_path}")


def validate_compiled_snippet_sync(errors: list[str]) -> None:
    cpp_reference = read_text(STREAMING_REFERENCE_PATH, errors)
    streaming_header = read_text(STREAMING_EXAMPLE_HEADER_PATH, errors)
    documented_header = extract_marked_cpp_block(cpp_reference, "COMPILED STREAMING HEADER", errors)
    if streaming_header and documented_header and streaming_header.strip() != documented_header.strip():
        add_error(errors, "The documented streaming header differs from the native sample compiled by skill contracts.")

    streaming_source = read_text(STREAMING_EXAMPLE_SOURCE_PATH, errors)
    documented_source = extract_marked_cpp_block(cpp_reference, "COMPILED STREAMING SOURCE", errors)
    if streaming_source and documented_source and streaming_source.strip() != documented_source.strip():
        add_error(errors, "The documented streaming implementation differs from the native sample compiled by skill contracts.")


def main() -> int:
    errors: list[str] = []
    for skill_name, references in SKILL_PACKAGES.items():
        validate_skill_package(skill_name, references, errors)
    validate_native_contract_wiring(errors)
    validate_compiled_snippet_sync(errors)

    if errors:
        print("UnrealAI skill lint failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(
        f"UnrealAI portable skill lint passed for {len(SKILL_PACKAGES)} skills. "
        "Run Scripts/ci/run_skill_contracts.py for compiled and behavioral validation."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
