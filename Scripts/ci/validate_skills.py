#!/usr/bin/env python3
"""Validate repository-local UnrealAI skills against the current public API."""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(
    os.environ.get("UNREALAI_REPOSITORY_ROOT", Path(__file__).resolve().parents[2])
).resolve()
SKILLS_ROOT = REPOSITORY_ROOT / ".agents" / "skills"
PUBLIC_SOURCE = "\n".join(
    path.read_text(encoding="utf-8")
    for path in sorted((REPOSITORY_ROOT / "Source" / "UnrealAI" / "Public").glob("*.h"))
)
SETTINGS_SOURCE = (REPOSITORY_ROOT / "Source/UnrealAI/Private/UnrealAISettings.cpp").read_text(encoding="utf-8")
CLIENT_SOURCE = (REPOSITORY_ROOT / "Source/UnrealAI/Private/UnrealAIClient.cpp").read_text(encoding="utf-8")
AUTOMATION_TEST_SOURCE = (
    REPOSITORY_ROOT / "Source/UnrealAI/Private/Tests/UnrealAIAutomationTests.cpp"
).read_text(encoding="utf-8")

SKILL_CONTRACTS = {
    "unrealai-cpp": {
        "reference": "references/cpp-api.md",
        "symbols": {
            "UUnrealAIClient",
            "UUnrealAIProviders",
            "ConfigureFromSettings",
            "CreateChatCompletion",
            "StreamChatCompletion",
            "CancelRequest",
            "OpenAICompatibleFromProfile",
            "UUnrealAIChatComponent",
            "SendPrompt",
            "SendMessages",
            "SendPromptStream",
            "SendMessagesStream",
            "CancelActiveStream",
            "FUnrealAIChatRequest",
            "FUnrealAIChatResponse",
            "FUnrealAIError",
            "FUnrealAIRequestHandle",
            "FUnrealAIChatStreamEvent",
            "FUnrealAIChatStreamResult",
            "EUnrealAIChatStreamEventType",
            "EUnrealAIChatStreamStatus",
            "GetFirstChoiceContent",
            "MakeJsonObjectResponseFormat",
            "MakeStrictJsonSchemaResponseFormat",
        },
        "claims": {
            "UPROPERTY",
            "ConfigureFromSettings",
            "CreateChatCompletion",
            "StreamChatCompletion",
            "CancelRequest",
            "GetFirstChoiceContent",
            "bStream",
            "TextDelta",
            "ProviderEvent",
            "Cancelled",
            "Anthropic",
            "Gemini",
        },
    },
    "unrealai-blueprints": {
        "reference": "references/blueprint-api.md",
        "symbols": {
            "CreateChatCompletion",
            "UUnrealAIChatStreamAsyncAction",
            "StreamChatCompletion",
            "MakeChatMessage",
            "MakeSimpleChatRequest",
            "GetFirstChoiceContent",
            "MakeJsonObjectResponseFormat",
            "MakeStrictJsonSchemaResponseFormat",
            "ResolveProviderConfig",
            "ReloadProjectEnvFile",
            "UUnrealAIChatComponent",
            "SendPrompt",
            "SendMessages",
            "SendPromptStream",
            "SendMessagesStream",
            "CancelActiveStream",
            "OnChatCompleted",
            "OnChatFailed",
            "OnChatStreamEvent",
            "OnChatStreamCompleted",
            "OnChatStreamFailed",
            "OnChatStreamCancelled",
            "EUnrealAIProviderApi",
        },
        "claims": {
            "Create Chat Completion (UnrealAI)",
            "Stream Chat Completion (UnrealAI)",
            "Make Simple Chat Request",
            "Get First Choice Content",
            "UnrealAIChatComponent",
            "Completed",
            "Failed",
            "Cancelled",
            "Async Action",
            "Text Delta",
            "Provider Event",
            "Has Content",
            "Anthropic",
            "Gemini",
        },
    },
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


def validate_skill(skill_name: str, contract: dict[str, object], errors: list[str]) -> None:
    skill_directory = SKILLS_ROOT / skill_name
    skill_path = skill_directory / "SKILL.md"
    reference_path = skill_directory / str(contract["reference"])
    openai_yaml_path = skill_directory / "agents/openai.yaml"

    expected_files = (skill_path, reference_path, openai_yaml_path)
    for path in expected_files:
        if not path.is_file():
            add_error(errors, f"Required skill file is missing: {path.relative_to(REPOSITORY_ROOT)}")
    if not all(path.is_file() for path in expected_files):
        return

    skill_content = read_text(skill_path, errors)
    reference_content = read_text(reference_path, errors)
    combined_content = f"{skill_content}\n{reference_content}"
    frontmatter = parse_frontmatter(skill_path, skill_content, errors)

    if frontmatter.get("name") != skill_name:
        add_error(errors, f"{skill_path.relative_to(REPOSITORY_ROOT)} frontmatter name must be {skill_name}.")
    description = frontmatter.get("description", "")
    if len(description) < 40 or "TODO" in description:
        add_error(errors, f"{skill_path.relative_to(REPOSITORY_ROOT)} needs a specific description.")

    if "TODO" in combined_content or "[TODO" in combined_content:
        add_error(errors, f"{skill_name} contains an unfinished scaffold placeholder.")

    expected_reference_link = f"]({contract['reference']})"
    if expected_reference_link not in skill_content:
        add_error(errors, f"{skill_name} does not route to {contract['reference']}.")

    validate_links(skill_directory, [skill_path, reference_path], errors)
    validate_openai_yaml(skill_name, openai_yaml_path, errors)

    for symbol in sorted(contract["symbols"]):
        if symbol not in PUBLIC_SOURCE:
            add_error(errors, f"{skill_name} expects public API symbol that is missing: {symbol}")

    for claim in sorted(contract["claims"]):
        if claim not in combined_content:
            add_error(errors, f"{skill_name} is missing required API guidance: {claim}")

    for forbidden_text in FORBIDDEN_PORTABILITY_TEXT:
        if forbidden_text in combined_content:
            add_error(errors, f"{skill_name} contains platform-specific instruction: {forbidden_text}")


def validate_shared_api_claims(errors: list[str]) -> None:
    settings_contract = {
        "OpenAI",
        "https://api.openai.com/v1",
        "gpt-5.6-luna",
        "OPENAI_API_KEY",
        "XAI",
        "https://api.x.ai/v1",
        "grok-4.6",
        "XAI_API_KEY",
        "XAI_BASE_URL",
        "XAI_MODEL",
        "Anthropic",
        "https://api.anthropic.com/v1",
        "claude-sonnet-5",
        "ANTHROPIC_API_KEY",
        "ANTHROPIC_BASE_URL",
        "ANTHROPIC_MODEL",
        "Gemini",
        "https://generativelanguage.googleapis.com/v1beta",
        "gemini-3.7-flash",
        "GEMINI_API_KEY",
        "GEMINI_BASE_URL",
        "GEMINI_MODEL",
    }
    for source_value in settings_contract:
        if f'TEXT("{source_value}")' not in SETTINGS_SOURCE:
            add_error(errors, f"Provider setting used by the skills is missing from source: {source_value}")

    stream_client_contract = {
        "StreamChatCompletion",
        "CancelRequest",
        "SetResponseBodyReceiveStreamDelegateV2",
        "EUnrealAIChatStreamStatus::Completed",
        "EUnrealAIChatStreamStatus::Failed",
        "EUnrealAIChatStreamStatus::Cancelled",
    }
    for source_value in stream_client_contract:
        if source_value not in CLIENT_SOURCE:
            add_error(errors, f"Streaming behavior used by the skills is missing from the client: {source_value}")

    stream_public_contract = {
        "DeprecatedProperty",
        'DisplayName = "Stream Chat Completion (UnrealAI)"',
        "FUnrealAIChatStreamEvent",
        "FUnrealAIChatStreamResult",
        "SendPromptStream",
        "SendMessagesStream",
        "CancelActiveStream",
    }
    for source_value in stream_public_contract:
        if source_value not in PUBLIC_SOURCE:
            add_error(errors, f"Streaming API used by the skills is missing from public headers: {source_value}")

    if '"UnrealAI.Blueprint.Surface"' not in AUTOMATION_TEST_SOURCE:
        add_error(errors, "The Blueprint skill requires the native Blueprint surface automation contract.")

    for test_id in ("UnrealAI.Streaming.SseParser", "UnrealAI.Streaming.ProviderAdapters"):
        if f'"{test_id}"' not in AUTOMATION_TEST_SOURCE:
            add_error(errors, f"The skills require the native streaming automation contract: {test_id}")

    if 'DisplayName = "Create Chat Completion (UnrealAI)"' not in PUBLIC_SOURCE:
        add_error(errors, "The Blueprint skill's async-node display name no longer matches the public header.")

    cpp_reference = read_text(SKILLS_ROOT / "unrealai-cpp/references/cpp-api.md", errors)
    if cpp_reference:
        ownership = cpp_reference.find("UPROPERTY()")
        creation = cpp_reference.find("UnrealAIClient = UUnrealAIProviders::OpenAI(this, ConfigError);")
        configuration_check = cpp_reference.find("if (!UnrealAIClient)", creation + 1)
        request = cpp_reference.find("CreateChatCompletion", configuration_check + 1)
        error_check = cpp_reference.find("if (Error.bIsError)")
        response_read = cpp_reference.find("GetFirstChoiceContent", error_check + 1)
        if min(ownership, creation, configuration_check, request, error_check, response_read) < 0:
            add_error(errors, "The C++ reference is missing a required ownership or request-handling step.")
        elif not (ownership < creation < configuration_check < request and error_check < response_read):
            add_error(errors, "The C++ reference demonstrates an unsafe request or response order.")


def main() -> int:
    errors: list[str] = []
    for skill_name, contract in SKILL_CONTRACTS.items():
        validate_skill(skill_name, contract, errors)
    validate_shared_api_claims(errors)

    if errors:
        print("UnrealAI skill validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(f"UnrealAI skill validation passed for {len(SKILL_CONTRACTS)} skills.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
