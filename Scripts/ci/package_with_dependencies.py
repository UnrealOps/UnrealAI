#!/usr/bin/env python3
"""Prepare a strict Unreal package command with project-local plugin dependencies.

Preparation never starts Unreal or modifies the engine. Run the emitted command
through ushell (.uat) on a matching native host. Stock BuildPlugin packaging and
filters remain authoritative; only host-project dependency discovery changes.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def prepare(engine: Path, plugin: Path, dependencies: list[Path], output: Path,
            platform: str, architecture: str = "", editor_only: bool = False,
            engine_association: str = "", editor_modules: bool = False) -> list[str]:
    if output.exists():
        raise ValueError("Packaging workspace must be fresh")
    engine = engine.resolve()
    plugin = plugin.resolve()
    inputs = [plugin, *(item.resolve() for item in dependencies)]
    names = []
    module_names = []
    for source in inputs:
        descriptors = list(source.glob('*.uplugin'))
        if len(descriptors) != 1:
            raise ValueError(f"Expected one descriptor in {source}")
        name = descriptors[0].stem
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', name) or name in names:
            raise ValueError("Invalid or duplicate plugin name")
        names.append(name)
        descriptor = json.loads(descriptors[0].read_text())
        module_names.extend(m['Name'] for m in descriptor.get('Modules', []))
    if architecture and not re.fullmatch(r'[A-Za-z0-9_+.-]+', architecture):
        raise ValueError('Invalid architecture')
    if editor_modules and not editor_only:
        raise ValueError('Module-only validation requires --editor-only')
    project_dir = output.resolve() / 'HostProject'
    for source, name in zip(inputs, names):
        shutil.copytree(source, project_dir / 'Plugins' / name,
                        ignore=shutil.ignore_patterns('.git', '.agents', 'Addons',
                            'Binaries', 'Intermediate', 'Saved', 'DerivedDataCache', '__pycache__', '.env*'))
    for source, name in zip(inputs, names):
        if (source/'.env.example').is_file():
            shutil.copy2(source/'.env.example', project_dir/'Plugins'/name/'.env.example')
    if editor_modules:
        # Disable PCH/unity in staged plugin rules only. Global flags on a source
        # engine would rebuild the whole engine instead of testing SDK includes.
        for rules in (project_dir/'Plugins').glob('*/Source/*/*.Build.cs'):
            text=rules.read_text()
            text=re.sub(r'PCHUsage\s*=\s*PCHUsageMode\.\w+;', 'PCHUsage = PCHUsageMode.NoPCHs;', text)
            text=re.sub(r'bUseUnity\s*=\s*true;', 'bUseUnity = false;', text)
            if 'bUseUnity = false;' not in text:
                text=text.replace('PCHUsage = PCHUsageMode.NoPCHs;', 'PCHUsage = PCHUsageMode.NoPCHs;\n        bUseUnity = false;', 1)
            rules.write_text(text)
    project = project_dir / 'PackageHost.uproject'
    project.write_text(json.dumps({'FileVersion': 3, 'EngineAssociation': engine_association,
        'DisableEnginePluginsByDefault': True,
        'Plugins': [{'Name': name, 'Enabled': True} for name in names]}, indent=2)+'\n')
    automation = project_dir / 'Build' / 'UnrealAIPackaging'
    automation.mkdir(parents=True)
    shutil.copy2(ROOT / 'Scripts/Automation/PackageUnrealAIPlugin.Automation.cs', automation)
    document = ET.Element('Project', Sdk='Microsoft.NET.Sdk')
    properties = ET.SubElement(document, 'PropertyGroup')
    for key, value in {'TargetFramework': 'net8.0', 'OutputType': 'Library',
                       'AssemblyName': 'UnrealAIPackaging.Automation',
                       'EnableDefaultCompileItems': 'true',
                       'OutputPath': str(project_dir/'Binaries/UnrealAIPackaging'),
                       'AppendTargetFrameworkToOutputPath': 'false'}.items():
        ET.SubElement(properties, key).text = value
    references = ET.SubElement(document, 'ItemGroup')
    binary_root = engine/'Engine/Binaries/DotNET/AutomationTool'
    for assembly in ('AutomationUtils.Automation', 'AutomationScripts.Automation', 'UnrealBuildTool', 'EpicGames.Core', 'EpicGames.Build', 'Microsoft.Extensions.Logging.Abstractions'):
        matches = list(binary_root.rglob(assembly+'.dll'))
        if not matches and assembly == 'UnrealBuildTool':
            matches = list((engine/'Engine/Binaries/DotNET/UnrealBuildTool').glob(assembly+'.dll'))
        if not matches:
            raise ValueError(f'Engine automation assembly missing: {assembly}')
        reference = ET.SubElement(references, 'Reference', Include=assembly)
        ET.SubElement(reference, 'HintPath').text = str(matches[0])
        ET.SubElement(reference, 'Private').text = 'false'
    ET.ElementTree(document).write(automation/'UnrealAIPackaging.Automation.csproj', encoding='unicode')
    command = ['PackageUnrealAIPlugin', f'-ScriptsForProject={project}',
        f'-HostProject={project}', f'-Plugin={project_dir / "Plugins" / names[0] / (names[0]+".uplugin")}',
        f'-Package={output.resolve()/"Package"/names[0]}', f'-TargetPlatform={platform}']
    if architecture:
        command.append(f'-Architecture={architecture}')
    if editor_only:
        command.append('-EditorOnly')
    if editor_modules:
        command.append('-EditorModules='+'+'.join(module_names))
    (output/'PackageCommand.json').write_text(json.dumps(command, indent=2)+'\n')
    (output/'Qualification.json').write_text(json.dumps({'plugins': names, 'platform': platform,
        'architecture': architecture, 'editor_only': editor_only, 'module_only': editor_modules,
        'result': 'prepared'}, indent=2)+'\n')
    return command


def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, default=ROOT)
    parser.add_argument('--dependency', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--platform', choices=('Mac','Win64','Linux'), required=True)
    parser.add_argument('--architecture', default='')
    parser.add_argument('--engine-association', default='')
    parser.add_argument('--editor-only', action='store_true')
    parser.add_argument('--editor-modules', action='store_true')
    args=parser.parse_args()
    print(json.dumps(prepare(args.engine,args.plugin,args.dependency,args.output,args.platform,
        args.architecture,args.editor_only,args.engine_association,args.editor_modules)))

if __name__=='__main__':
    main()
