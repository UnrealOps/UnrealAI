#!/usr/bin/env python3
"""Prepare an isolated base/auth/experimental SDK consumer for native ushell validation."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
IGNORED = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", ".git", "__pycache__"}
COPY_ROOTS = {"Source", "Config", "Resources", "Content", "Documentation"}


def copy_plugin(source: Path, destination: Path, name: str) -> None:
    if not (source / f"{name}.uplugin").is_file():
        raise ValueError(f"Missing {name} plugin descriptor")
    destination.mkdir(parents=True)
    shutil.copy2(source / f"{name}.uplugin", destination / f"{name}.uplugin")
    for directory in COPY_ROOTS:
        if (source / directory).is_dir():
            shutil.copytree(source / directory, destination / directory,
                            ignore=shutil.ignore_patterns(*IGNORED, ".env*"))


def prepare(output: Path, mode: str, sdk: Path = ROOT, engine_association: str = "", addons_root: Path = ROOT / "Addons") -> Path:
    output = output.resolve()
    if output.exists():
        raise ValueError("Consumer output must be a fresh directory.")
    names = ["UnrealAI"]
    if mode in ("auth", "experimental"):
        names.append("UnrealAIAuth")
    if mode == "experimental":
        names.append("UnrealAIExperimentalAccess")
    for name in names:
        source = sdk if name == "UnrealAI" else addons_root / name
        copy_plugin(source, output / "Plugins" / name, name)
    project = output / "UnrealAIConsumer.uproject"
    project.write_text(json.dumps({"FileVersion": 3, "EngineAssociation": engine_association, "DisableEnginePluginsByDefault": True,
        "Modules": [{"Name": "UnrealAIConsumer", "Type": "Runtime", "LoadingPhase": "Default"}],
        "Plugins": [{"Name": name, "Enabled": True} for name in names]}, indent=2) + "\n")
    module = output / "Source" / "UnrealAIConsumer"
    module.mkdir(parents=True)
    (module / "UnrealAIConsumer.Build.cs").write_text('''using UnrealBuildTool;
public class UnrealAIConsumer : ModuleRules
{
    public UnrealAIConsumer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UnrealAI", "UnrealAIAccess" });
        PrivateDependencyModuleNames.Add("Projects");
    }
}
''')
    (module / "UnrealAIConsumer.cpp").write_text('''#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "UnrealAIExecutionService.h"
#include "Models/UnrealAIProviderCatalog.h"
#include "Auth/UnrealAICredentialBrokerFactory.h"
IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, UnrealAIConsumer, "UnrealAIConsumer");
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUnrealAIConsumerIsolationTest, "UnrealAI.Consumer.OptionalDependencyIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUnrealAIConsumerIsolationTest::RunTest(const FString& Parameters)
{
    const TSet<FString> ExpectedPlugins{ ''' + ', '.join('TEXT("' + name + '")' for name in names) + ''' };
    TSet<FString> EnabledProjectPlugins;
    for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
    {
        if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project)
        {
            EnabledProjectPlugins.Add(Plugin->GetName());
            TestTrue(TEXT("Every enabled project plugin belongs to the selected SDK configuration"), ExpectedPlugins.Contains(Plugin->GetName()));
        }
    }
    TestEqual(TEXT("The project enables exactly the selected SDK plugins"), EnabledProjectPlugins.Num(), ExpectedPlugins.Num());
    TestEqual(TEXT("Optional auth matches the selected consumer mode"), IUnrealAICredentialBrokerFactory::Get().IsValid(), ''' + ("false" if mode == "base" else "true") + ''');
    const auto Service = MakeShared<FUnrealAIExecutionService, ESPMode::ThreadSafe>();
    TestFalse(TEXT("Native convenience client starts unconfigured"), Service->IsConfigured());
    return true;
}
#endif
''')
    for suffix, kind in (("", "Game"), ("Editor", "Editor"), ("Server", "Server")):
        name = "UnrealAIConsumer" + suffix
        (output / "Source" / f"{name}.Target.cs").write_text('''using UnrealBuildTool;
public class ''' + name + '''Target : TargetRules
{
    public ''' + name + '''Target(TargetInfo Target) : base(Target)
    {
        Type = TargetType.''' + kind + ''';
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        ExtraModuleNames.Add("UnrealAIConsumer");
    }
}
''')
    (output / "Automation.txt").write_text('-ExecCmds="Automation RunTests UnrealAI.; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="' + str(output / "Report") + '"\n')
    (output / "ConsumerMode.json").write_text(json.dumps({"mode": mode, "plugins": names}, indent=2) + "\n")
    return project


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("base", "auth", "experimental"), required=True)
    parser.add_argument("--sdk", type=Path, default=ROOT, help="SDK source or an already packaged SDK directory")
    parser.add_argument("--engine-association", default="", help="EngineAssociation from an existing project using the selected engine")
    parser.add_argument("--addons-root", type=Path, default=ROOT / "Addons", help="Source or packaged addon collection")
    args = parser.parse_args()
    print(prepare(args.output, args.mode, args.sdk, args.engine_association, args.addons_root))


if __name__ == "__main__":
    main()
