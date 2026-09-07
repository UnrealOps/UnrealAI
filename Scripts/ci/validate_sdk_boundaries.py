#!/usr/bin/env python3
"""Validate the SDK's native ownership boundary and optional plugin dependency graph."""
from __future__ import annotations

import json
from pathlib import Path
import re

BASE_MODULES = {"UnrealAI", "UnrealAIAccess", "UnrealAITransport"}
ADDONS = {"UnrealAIAuth": {"UnrealAIAuth", "UnrealAIAuthEditor"},
          "UnrealAIExperimentalAccess": {"UnrealAIAuthOpenAI", "UnrealAIAuthXAI"}}
PLUGIN_DEPENDENCIES = {"UnrealAIAuth": {"UnrealAI"},
                      "UnrealAIExperimentalAccess": {"UnrealAI", "UnrealAIAuth"}}
MODULE_DEPENDENCIES = {
    "UnrealAI": {"Core", "CoreUObject", "Engine", "DeveloperSettings", "HTTP", "Json", "JsonUtilities",
                 "UnrealAIAccess", "UnrealAITransport"},
    "UnrealAIAccess": {"Core", "CoreUObject", "Json"},
    "UnrealAITransport": {"Core", "HTTP", "UnrealAIAccess"},
    "UnrealAIAuth": {"Core", "Sockets", "UnrealAI", "UnrealAIAccess", "UnrealAITransport", "OpenSSL"},
    "UnrealAIAuthEditor": {"Core", "CoreUObject", "UnrealAIAccess", "UnrealAIAuth", "Slate", "SlateCore", "ToolMenus"},
    "UnrealAIAuthOpenAI": {"Core", "Json", "UnrealAIAccess", "UnrealAI", "UnrealAIAuth"},
    "UnrealAIAuthXAI": {"Core", "Json", "UnrealAIAccess", "UnrealAI", "UnrealAIAuth"},
}
MODULE_REFERENCE = re.compile(
    r'(?:(?:Public|Private)(?:Dependency|IncludePath)ModuleNames|DynamicallyLoadedModuleNames)'
    r'\s*\.\s*Add(?:Range)?\s*\((.*?)\)\s*;'
    r'|AddEngineThirdPartyPrivateStaticDependencies\s*\((.*?)\)\s*;', re.S)


def validate_module_dependencies(root: Path, path: Path, errors: list[str]) -> None:
    module = path.name.removesuffix(".Build.cs")
    if module not in MODULE_DEPENDENCIES:
        errors.append(f"SDK contains an undeclared module: {path.relative_to(root)}")
        return
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)
    dependencies = set()
    for match in MODULE_REFERENCE.finditer(source):
        dependencies.update(re.findall(r'"([^"\n]+)"', match.group(1) or match.group(2)))
    unexpected = dependencies - MODULE_DEPENDENCIES[module]
    if unexpected:
        errors.append(f"SDK module has unapproved dependencies {sorted(unexpected)}: {path.relative_to(root)}")


def validate(root: Path, errors: list[str]) -> None:
    descriptor = json.loads((root / "UnrealAI.uplugin").read_text())
    modules = descriptor.get("Modules", [])
    if {item.get("Name") for item in modules} != BASE_MODULES or any(item.get("Type") != "Runtime" for item in modules):
        errors.append("Base SDK must contain exactly the three runtime modules.")
    if descriptor.get("Plugins"):
        errors.append("Base SDK cannot depend on other plugins.")
    for path in (root / "Source").rglob("*.Build.cs"):
        validate_module_dependencies(root, path, errors)
    for addon, expected in ADDONS.items():
        plugin = root / "Addons" / addon
        data = json.loads((plugin / f"{addon}.uplugin").read_text())
        if data.get("EnabledByDefault") is not False or {item.get("Name") for item in data["Modules"]} != expected:
            errors.append(f"Optional plugin module topology or default changed: {addon}")
        references = data.get("Plugins", [])
        if ({item.get("Name") for item in references} != PLUGIN_DEPENDENCIES[addon]
                or any(item.get("Enabled") is not True for item in references)):
            errors.append(f"Optional SDK plugin must declare only its required SDK dependencies: {addon}")
        if addon == "UnrealAIExperimentalAccess":
            for module in data["Modules"]:
                if "Server" not in module.get("TargetDenyList", []) or "Shipping" not in module.get("TargetConfigurationDenyList", []):
                    errors.append("Experimental access must remain excluded from Server and Shipping.")
        for path in (plugin / "Source").rglob("*.Build.cs"):
            validate_module_dependencies(root, path, errors)
    contracts = {
        "Source/UnrealAIAccess/Public/Auth/UnrealAIProviderAccess.h": ["FUnrealAISecretValue final", "FUnrealAICredentialDestination", "IUnrealAIRefreshableCredentialBroker"],
        "Source/UnrealAIAccess/Public/Models/UnrealAIModelTypes.h": ["MaxRetainedRequestBytes", "FUnrealAIModelRequest", "IUnrealAIModelContinuation"],
        "Source/UnrealAI/Public/Models/UnrealAIModelProvider.h": ["IsLogicallyComplete", "IsPhysicallySettled", "OnPhysicalSettled"],
        "Source/UnrealAI/Private/Execution/UnrealAINativeSseModelProvider.cpp": ["PollDeadline", "CommitHttpAdmission", "NotifyPhysicalSettled", "PhysicalPermit", "client_provider_secret_denied"],
        "Source/UnrealAI/Public/Models/UnrealAIProviderCatalog.h": ["MaximumProviders", "IUnrealAIApiKeyProvisioner"],
    }
    for relative, markers in contracts.items():
        path = root / relative
        source = path.read_text() if path.is_file() else ""
        for marker in markers:
            if marker not in source:
                errors.append(f"SDK ownership/lifecycle contract missing {marker}: {relative}")


if __name__ == "__main__":
    import sys
    failures: list[str] = []
    validate(Path(__file__).resolve().parents[2], failures)
    if failures:
        print("\n".join(failures), file=sys.stderr)
        raise SystemExit(1)
    print("UnrealAI native and optional-plugin boundaries passed.")
