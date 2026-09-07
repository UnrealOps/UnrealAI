"""Protect the SDK's generic module boundary without naming consumer projects."""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from validate_sdk_boundaries import validate_module_dependencies


class ModuleDependencyTests(unittest.TestCase):
    def check_rules(self, module: str, source: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rules = root / f"{module}.Build.cs"
            rules.write_text(source)
            errors: list[str] = []
            validate_module_dependencies(root, rules, errors)
            return errors

    def test_accepts_sdk_and_engine_dependencies(self) -> None:
        self.assertEqual([], self.check_rules("UnrealAI", '''
            PublicDependencyModuleNames.AddRange(new[] { "Core", "UnrealAIAccess" });
            PrivateDependencyModuleNames.Add("HTTP");
        '''))
        self.assertEqual([], self.check_rules("UnrealAIAuth", '''
            AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
        '''))

    def test_rejects_consumer_dependencies_in_every_sdk_layer(self) -> None:
        for module in ("UnrealAI", "UnrealAIAccess", "UnrealAITransport", "UnrealAIAuth", "UnrealAIAuthXAI"):
            with self.subTest(module=module):
                errors = self.check_rules(module, 'PrivateDependencyModuleNames.Add("ConsumerRuntime");')
                self.assertTrue(errors)
                self.assertIn("ConsumerRuntime", errors[0])

    def test_rejects_dynamic_and_include_path_dependencies(self) -> None:
        for collection in ("PublicIncludePathModuleNames", "PrivateIncludePathModuleNames", "DynamicallyLoadedModuleNames"):
            with self.subTest(collection=collection):
                errors = self.check_rules("UnrealAI", collection + '.AddRange(new[] { "ConsumerRuntime" });')
                self.assertTrue(errors)
                self.assertIn("ConsumerRuntime", errors[0])

    def test_rejects_editor_or_auth_dependency_in_base(self) -> None:
        for module in ("UnrealAIAuth", "UnrealEd", "Slate"):
            with self.subTest(module=module):
                self.assertTrue(self.check_rules("UnrealAI", f'PrivateDependencyModuleNames.Add("{module}");'))

    def test_ignores_commented_dependencies(self) -> None:
        self.assertEqual([], self.check_rules("UnrealAI", '''
            // PrivateDependencyModuleNames.Add("ConsumerRuntime");
            /* PublicDependencyModuleNames.Add("ConsumerTools"); */
            PublicDependencyModuleNames.Add("Core");
        '''))

    def test_rejects_undeclared_sdk_module(self) -> None:
        self.assertTrue(self.check_rules("ConsumerRuntime", 'PublicDependencyModuleNames.Add("Core");'))


if __name__ == "__main__":
    unittest.main()
