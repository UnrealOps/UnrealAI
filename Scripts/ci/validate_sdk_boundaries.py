#!/usr/bin/env python3
"""Validate the SDK's native ownership boundary and optional plugin dependency graph."""
from __future__ import annotations

import json
from pathlib import Path
import re

BASE_MODULES = {"UnrealAI", "UnrealAIAccess", "UnrealAITransport"}
ADDONS = {"UnrealAIAuth": {"UnrealAIAuth", "UnrealAIAuthEditor"},
          "UnrealAIExperimentalAccess": {"UnrealAIAuthOpenAI", "UnrealAIAuthXAI"}}


def validate(root: Path, errors: list[str]) -> None:
    descriptor = json.loads((root / "UnrealAI.uplugin").read_text())
    modules = descriptor.get("Modules", [])
    if {item.get("Name") for item in modules} != BASE_MODULES or any(item.get("Type") != "Runtime" for item in modules):
        errors.append("Base SDK must contain exactly the three runtime modules.")
    if descriptor.get("Plugins"):
        errors.append("Base SDK cannot depend on optional authentication or agent plugins.")
    for path in (root / "Source").rglob("*.Build.cs"):
        source = path.read_text()
        if re.search(r'"(?:AutonomousAgents\w*|UnrealAIAuth\w*|UnrealEd|Slate|SlateCore)"', source):
            errors.append(f"Base runtime links an optional implementation or editor module: {path.relative_to(root)}")
    for addon, expected in ADDONS.items():
        plugin = root / "Addons" / addon
        data = json.loads((plugin / f"{addon}.uplugin").read_text())
        if data.get("EnabledByDefault") is not False or {item.get("Name") for item in data["Modules"]} != expected:
            errors.append(f"Optional plugin module topology or default changed: {addon}")
        if addon == "UnrealAIExperimentalAccess":
            for module in data["Modules"]:
                if "Server" not in module.get("TargetDenyList", []) or "Shipping" not in module.get("TargetConfigurationDenyList", []):
                    errors.append("Experimental access must remain excluded from Server and Shipping.")
        for path in (plugin / "Source").rglob("*.Build.cs"):
            if re.search(r'"AutonomousAgents\w*"', path.read_text()):
                errors.append(f"Optional SDK plugin depends on the agent framework: {path.relative_to(root)}")
    for path in (root / "Source").rglob("*.h"):
        source = path.read_text()
        if re.search(r'#include\s+"(?:Models/Agent|Runtime/Agent|Identity/Agent|Perception/Agent)', source):
            errors.append(f"SDK public contract includes agent/world code: {path.relative_to(root)}")
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
