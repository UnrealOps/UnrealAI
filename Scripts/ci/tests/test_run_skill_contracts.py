from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "run_skill_contracts.py"
SPEC = importlib.util.spec_from_file_location("run_skill_contracts", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Unable to load {SCRIPT}")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class StageSampleProjectTests(unittest.TestCase):
    def test_stages_source_without_generated_output_and_rewrites_plugin_path(self) -> None:
        with tempfile.TemporaryDirectory(prefix="unrealai-stage-test-") as temporary_directory:
            temporary_root = Path(temporary_directory)
            source_directory = temporary_root / "SourceProject"
            source_directory.mkdir()
            source_project = source_directory / "Sample.uproject"
            source_project.write_text(
                json.dumps({"FileVersion": 3, "AdditionalPluginDirectories": ["../../.."]}),
                encoding="utf-8",
            )
            (source_directory / "Source").mkdir()
            (source_directory / "Source" / "Example.cpp").write_text("// source\n", encoding="utf-8")
            (source_directory / "Binaries").mkdir()
            (source_directory / "Binaries" / "generated.bin").write_bytes(b"generated")
            (source_directory / "Saved").mkdir()
            (source_directory / "Saved" / "log.txt").write_text("generated\n", encoding="utf-8")

            output_directory = temporary_root / "Output"
            output_directory.mkdir()
            staged_project = RUNNER.stage_sample_project(source_project, output_directory)

            self.assertTrue(staged_project.is_file())
            self.assertTrue((staged_project.parent / "Source" / "Example.cpp").is_file())
            self.assertFalse((staged_project.parent / "Binaries").exists())
            self.assertFalse((staged_project.parent / "Saved").exists())
            staged_descriptor = json.loads(staged_project.read_text(encoding="utf-8"))
            self.assertEqual(
                staged_descriptor["AdditionalPluginDirectories"],
                [str(output_directory / "ExternalPlugins")],
            )
            staged_plugin = output_directory / "ExternalPlugins" / "UnrealAI"
            self.assertTrue((staged_plugin / "UnrealAI.uplugin").is_file())
            self.assertTrue((staged_plugin / "Source" / "UnrealAI" / "UnrealAI.Build.cs").is_file())
            self.assertFalse((staged_plugin / "Binaries").exists())
            self.assertFalse((staged_plugin / "Samples").exists())

    def test_contract_consumes_sample_and_plugin_test_groups(self) -> None:
        contract = {
            "automationFilter": "Sample.Filter",
            "requiredSampleTests": ["Sample.One", "Sample.Two"],
            "pluginAutomationFilter": "Plugin.Filter",
            "requiredPluginTests": ["Plugin.One", "Plugin.Two"],
        }

        groups = RUNNER.contract_test_groups(contract)

        self.assertEqual(
            groups,
            [
                ("SampleAutomationReport", "Sample.Filter", ["Sample.One", "Sample.Two"]),
                ("PluginAutomationReport", "Plugin.Filter", ["Plugin.One", "Plugin.Two"]),
            ],
        )
        sample_command = RUNNER.report_check_command(Path("sample-report"), groups[0][2])
        plugin_command = RUNNER.report_check_command(Path("plugin-report"), groups[1][2])
        self.assertIn("Sample.One", sample_command)
        self.assertIn("Sample.Two", sample_command)
        self.assertIn("Plugin.One", plugin_command)
        self.assertIn("Plugin.Two", plugin_command)


if __name__ == "__main__":
    unittest.main()
